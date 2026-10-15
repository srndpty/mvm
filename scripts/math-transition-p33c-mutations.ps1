<#
.SYNOPSIS
    P3-3C: 合成器の source を 1 か所だけ書き換えた変異が、単体試験の意図した検査と、実際の MLT との
    比較の両方で検出されることを確かめる。

.DESCRIPTION
    変異ごとに、source を書き換える -> build -> 単体試験 (どの検査で落ちたかを見る) ->
    数式 group (準備済みの画像は中身のある範囲だけ) を製品の MLT の pre-YUV と比べる
    (画素の不一致として検出されること。書き出しの失敗や timeout ではないこと)。
    最後に必ず元の内容へ戻し、SHA-256 の一致を確かめてから build と試験をやり直す。
    作業 tree を書き換えるので、他の build・書き出しと同時に実行しない。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-transition-p33c-20261012'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$ucrt = 'C:\msys64\ucrt64\bin'
$env:PATH = "$ucrt;$(Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64');$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$cpp = Join-Path $repo 'src\media\math\math_group_compose.cpp'
$test = Join-Path $repo 'build\ucrt64-release\bin\mvm_test_math_group_compose.exe'
$harness = Join-Path $repo 'build\ucrt64-release\bin\mvm_math_score_export_parity.exe'
$out = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-mutation')
if (Test-Path -LiteralPath $out) { throw "出力先が既にあります: $out" }
New-Item -ItemType Directory -Force $out | Out-Null

$roiTest = '中央の字形: 範囲だけを持つ画像の合成は基準と同じ'
$mutations = @(
    @{ name = 'roi-origin-shifted'; fixture = 'cmp-layers-16'
        # 保存する画素を 1 列ずれた所から複製する (範囲の原点の誤り)。
        find = 'static_cast<std::size_t>(contentLeft) * 4U,'
        replace = 'static_cast<std::size_t>(contentLeft + 1) * 4U,'
        expect = $roiTest
    },
    @{ name = 'interpolation-halo-missing'; fixture = 'cmp-layers-16'
        # 左の texel が範囲の外のとき、右の texel も読まない (範囲の左の縁の 1 画素外が描かれない)。
        find = 'const bool rightStored = m + 1 >= layer.storedX && m + 1 < storedRight;'
        replace = 'const bool rightStored = leftStored && m + 1 < storedRight;'
        expect = $roiTest
    },
    @{ name = 'exterior-not-transparent'; fixture = 'cmp-layers-16'
        # 範囲の外の texel を、透明 (0, 0, 0, 0) ではなく不透明な黒として読む。
        find = 'const std::uint8_t kZeroTexel[4] = {0, 0, 0, 0};'
        replace = 'const std::uint8_t kZeroTexel[4] = {0, 0, 0, 255};'
        expect = $roiTest
    },
    @{ name = 'destination-alpha-divide'; fixture = 'cmp-layers-48'
        # 下の alpha の正規化を割り算にする (P3-1.2 の前の計算)。
        find = 'const float below = f32(static_cast<float>(pixel[3]) * f32(1.0f / 255.0f));'
        replace = 'const float below = f32(static_cast<float>(pixel[3]) / 255.0f);'
        expect = '下の alpha が 254 の画素が MLT の観測値と一致する'
    }
)
$original = [IO.File]::ReadAllText($cpp)
$before = (Get-FileHash -Algorithm SHA256 $cpp).Hash
$log = @("original sha256 $before")
$undetected = 0
try {
    foreach ($mutation in $mutations) {
        if (($original.Split($mutation.find).Count - 1) -ne 1) {
            $log += "$($mutation.name): 変異させる行が 1 か所ではありません (INVALID)"; ++$undetected; continue
        }
        [IO.File]::WriteAllText($cpp, $original.Replace($mutation.find, $mutation.replace))
        pwsh (Join-Path $repo 'scripts\build.ps1') -Target mvm_test_math_group_compose 2>&1 | Out-Null
        $built = $LASTEXITCODE
        pwsh (Join-Path $repo 'scripts\build.ps1') -Target mvm_math_score_export_parity 2>&1 | Out-Null
        $harnessBuilt = $LASTEXITCODE
        $text = (& $test 2>&1 | Out-String)
        $code = $LASTEXITCODE
        $text | Set-Content (Join-Path $out "$($mutation.name)-unit.txt") -Encoding utf8
        $intended = $text.Contains("FAIL: $($mutation.expect)")
        # 実際の MLT との比較。画素の不一致 (終了コード 13 または 14) として検出されること。
        $runOut = Join-Path $out "$($mutation.name)-mlt"
        New-Item -ItemType Directory -Force $runOut | Out-Null
        & $harness --project (Join-Path $repo $ProjectPath) --manim (Join-Path $env:USERPROFILE '.local\bin\manim.exe') `
            --out $runOut --ffprobe (Join-Path $ucrt 'ffprobe.exe') --fixture $mutation.fixture --cap-seconds 900 `
            --compare math-group --math-group-rule 1 --prepared-storage roi 2> (Join-Path $runOut 'harness.log') | Out-Null
        $mltExit = $LASTEXITCODE
        $compare = (Get-Content -Raw (Join-Path $runOut "$($mutation.fixture)-report.json") | ConvertFrom-Json).compare
        $pixelMismatch = $mltExit -in @(13, 14) -and $compare.frames_uncompared -eq 0 -and
        $compare.baseline_repeat_mismatches -eq 0 -and $compare.frames_exact -lt $compare.frames_expected
        $detected = $built -eq 0 -and $harnessBuilt -eq 0 -and $code -ne 0 -and $intended -and $pixelMismatch
        if (-not $detected) { ++$undetected }
        $log += "$($mutation.name): build=$built/$harnessBuilt 単体試験 exit=$code 意図した検査 '$($mutation.expect)' で失敗=$intended || MLT ($($mutation.fixture)) exit=$mltExit $($compare.verdict) 一致=$($compare.frames_exact)/$($compare.frames_expected) 画素差=$($compare.differing_pixels) 検出=$detected"
    }
} finally {
    [IO.File]::WriteAllText($cpp, $original)
}
$after = (Get-FileHash -Algorithm SHA256 $cpp).Hash
$log += "restored sha256 $after 一致=$($after -eq $before)"
pwsh (Join-Path $repo 'scripts\build.ps1') 2>&1 | Out-Null
$rebuilt = $LASTEXITCODE
$text = (& $test 2>&1 | Out-String)
$restoredExit = $LASTEXITCODE
$log += "復元後: build=$rebuilt 単体試験 exit=$restoredExit " + ((($text -split "`n") | Where-Object { $_ -match 'oracle と比べた' }) -join '').Trim()
$log | Set-Content (Join-Path $out 'mutation-log.txt') -Encoding utf8
$log
if ($undetected -ne 0 -or $after -ne $before -or $restoredExit -ne 0) { exit 1 }
exit 0
