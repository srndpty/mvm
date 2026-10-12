<#
.SYNOPSIS
    P2-B: transition の分け方ごとに、v4 全体の graph の同じ 3 区間の書き出し時間を測る。

.DESCRIPTION
    分け方 (variant) と区間 (window) は実行前に固定してある。結果を見て増減しない。
    すべて全画面 (FullFrame) の渡し方で、graph は Project 全体のまま 60 frame だけを書き出す。
    各 variant を -Runs 回ずつ実行し、回ごとに variant の順番を 1 つずつ回す (同じ variant がいつも
    先頭・末尾にならないようにして、cache や温度の偏りを減らす)。上限時間で打ち切った run も、
    失敗した run も残す。やり直して成功を選ぶことはしない。

    variant:
      none           分けない (製品の動作)
      64-8 など      変わる区間の上限 / 変わらない区間の下限
      64-8-chunk240  64/8 に加えて、変わらない区間を 240 frame ごとに刻む
      64-8-chunk60   同、60 frame ごと
    chunk の 2 つは、出力も 1 つの transition の keyframe も変えずに transition の総数だけを増やす。
    同時に有効な transition の数 (= layer の数) は変わらないので、graph の大きさに比例するコストを
    分けて見られる。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-export-p2b-20261012',
    [string]$Label = 'matrix',
    [int]$Runs = 3,
    [int]$CapSeconds = 60
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
$git = (Get-Command git -ErrorAction SilentlyContinue)?.Source
if (-not $git) { $git = 'C:\Program Files\Git\cmd\git.exe' }
foreach ($required in @($harness, $manim, $latex, $project, $ffprobe, $git)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}

$variants = @(
    @{ name = 'none'; args = @() }
    @{ name = '64-8'; args = @('--split-transitions', 1, '--split-policy', '64/8') }
    @{ name = '128-16'; args = @('--split-transitions', 1, '--split-policy', '128/16') }
    @{ name = '256-32'; args = @('--split-transitions', 1, '--split-policy', '256/32') }
    @{ name = '512-64'; args = @('--split-transitions', 1, '--split-policy', '512/64') }
    @{ name = '64-8-chunk240'; args = @('--split-transitions', 1, '--split-policy', '64/8/240') }
    @{ name = '64-8-chunk60'; args = @('--split-transitions', 1, '--split-policy', '64/8/60') }
)
$windows = @(@{ name = 'early'; range = '1200:60' }, @{ name = 'middle'; range = '3400:60' }, @{ name = 'late'; range = '6500:60' })

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDirectory = Join-Path (Join-Path $repo $OutputDirectory) "$stamp-$Label"
if (Test-Path -LiteralPath $runDirectory) { throw "出力先が既にあります: $runDirectory" }
New-Item -ItemType Directory -Force $runDirectory | Out-Null

$identity = [ordered]@{
    git_head = (& $git -C $repo rev-parse HEAD).Trim()
    git_status = @(& $git -C $repo status --short)
    runs_per_variant = $Runs
    cap_seconds = $CapSeconds
    variants = @($variants | ForEach-Object { [ordered]@{ name = $_.name; args = ($_.args -join ' ') } })
    windows = $windows
    sha256 = [ordered]@{}
}
$hashed = @($harness, (Join-Path $ucrt 'libmlt-7.dll'), 'C:\msys64\ucrt64\lib\mlt\libmltplus.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltqt6.dll', 'C:\msys64\ucrt64\lib\mlt\libmltavformat.dll',
    (Join-Path $ucrt 'avcodec-62.dll'), (Join-Path $ucrt 'libx264-165.dll'), $project,
    (Join-Path $repo 'src/media/mlt/mvm_mlt_export.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_export.h'),
    (Join-Path $repo 'src/app/timeline_export.cpp'), (Join-Path $repo 'tests/harness/math_score_export_parity.cpp'),
    (Join-Path $repo 'tests/harness/mlt_property_probe.h'), (Join-Path $repo 'scripts/math-export-p2b-matrix.ps1'))
foreach ($file in $hashed) {
    if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$outcomes = @()
$order = 0
foreach ($run in 1..$Runs) {
    foreach ($window in $windows) {
        # 回と区間ごとに、variant の並びを 1 つずつ回す。
        $rotated = @(0..($variants.Count - 1) | ForEach-Object { $variants[($_ + $order) % $variants.Count] })
        $order++
        foreach ($variant in $rotated) {
            $caseDirectory = Join-Path $runDirectory "$($window.name)/$($variant.name)/run$run"
            New-Item -ItemType Directory -Force $caseDirectory | Out-Null
            $arguments = @('--project', $project, '--manim', $manim, '--out', $caseDirectory, '--ffprobe', $ffprobe,
                '--fixture', 'v4-full', '--range', $window.range, '--cap-seconds', $CapSeconds,
                '--stall-seconds', 45, '--stack-samples', 0) + $variant.args
            & $harness @arguments 2> (Join-Path $caseDirectory 'harness.log') | Out-Null
            $outcomes += [pscustomobject]@{ window = $window.name; variant = $variant.name; run = $run; exit_code = $LASTEXITCODE; sequence = $outcomes.Count }
        }
        Write-Host ("run{0} {1} 完了" -f $run, $window.name)
    }
}
$outcomes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'outcomes.json') -Encoding utf8
$unusable = @($outcomes | Where-Object { $_.exit_code -notin 0, 9 })
Write-Host "結果: $runDirectory"
Write-Host ("計測に使える run: {0}/{1}" -f ($outcomes.Count - $unusable.Count), $outcomes.Count)
exit 0
