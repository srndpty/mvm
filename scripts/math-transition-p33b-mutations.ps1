<#
.SYNOPSIS
    P3-3B: 合成器の source を 1 か所だけ書き換えた変異が、単体試験の意図した検査で検出されることを
    確かめる。

.DESCRIPTION
    変異ごとに、source を書き換える -> build -> 単体試験 (失敗を期待し、どの検査で落ちたかを見る)。
    最後に必ず元の内容へ戻し、SHA-256 の一致を確かめてから build と試験をやり直す。
    作業 tree を書き換えるので、他の build・書き出しと同時に実行しない。
#>
[CmdletBinding()]
param([string]$OutputDirectory = 'build/math-transition-p33b-20261012')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$cpp = Join-Path $repo 'src\media\math\math_group_compose.cpp'
$header = Join-Path $repo 'src\media\math\math_group_compose.h'
$test = Join-Path $repo 'build\ucrt64-release\bin\mvm_test_math_group_compose.exe'
$out = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-mutation')
if (Test-Path -LiteralPath $out) { throw "出力先が既にあります: $out" }
New-Item -ItemType Directory -Force $out | Out-Null

$mutations = @(
    @{ name = 'destination-alpha-divide'; file = $cpp
        find = 'const float below = f32(static_cast<float>(pixel[3]) * f32(1.0f / 255.0f));'
        replace = 'const float below = f32(static_cast<float>(pixel[3]) / 255.0f);'
        expect = '下の alpha が 254 の画素が MLT の観測値と一致する'
    },
    @{ name = 'alpha-floor-zero'; file = $header
        find = 'constexpr int kMathGroupMinimumDestinationAlpha = 1;'
        replace = 'constexpr int kMathGroupMinimumDestinationAlpha = 0;'
        expect = 'alpha 0 の画素がある下地は対象外と判定する'
    },
    @{ name = 'alpha-floor-check-removed'; file = $cpp
        find = 'if (lowest < minimumDestinationAlpha) {'
        replace = 'if (false) {'
        expect = 'alpha 0 の画素がある下地を拒否し、何も書かない'
    },
    @{ name = 'alpha-minimum-ignores-last-row'; file = $cpp
        find = 'for (int row = 0; row < height; ++row) {' + "`n" + '        const std::uint8_t* line = destination + static_cast<std::size_t>(row) * stride;' + "`n" + '        std::uint8_t rowLowest = 255;'
        replace = 'for (int row = 0; row + 1 < height; ++row) {' + "`n" + '        const std::uint8_t* line = destination + static_cast<std::size_t>(row) * stride;' + "`n" + '        std::uint8_t rowLowest = 255;'
        expect = 'alpha 0 の画素がある下地は対象外と判定する'
    },
    @{ name = 'prepared-roi-one-column-short'; file = $cpp
        find = 'raster.roiWidth_ = right - left + 1;'
        replace = 'raster.roiWidth_ = right - left;'
        expect = '字形の範囲は画素から求まる'
    }
)
$originals = @{}
$before = @{}
foreach ($file in @($cpp, $header)) {
    $originals[$file] = [IO.File]::ReadAllText($file)
    $before[$file] = (Get-FileHash -Algorithm SHA256 $file).Hash
}
$log = @("original sha256 cpp=$($before[$cpp]) header=$($before[$header])")
$undetected = 0
try {
    foreach ($mutation in $mutations) {
        $original = $originals[$mutation.file]
        if (($original.Split($mutation.find).Count - 1) -ne 1) {
            $log += "$($mutation.name): 変異させる行が 1 か所ではありません (INVALID)"; ++$undetected; continue
        }
        [IO.File]::WriteAllText($mutation.file, $original.Replace($mutation.find, $mutation.replace))
        pwsh (Join-Path $repo 'scripts\build.ps1') -Target mvm_test_math_group_compose 2>&1 | Out-Null
        $built = $LASTEXITCODE
        $text = (& $test 2>&1 | Out-String)
        $code = $LASTEXITCODE
        [IO.File]::WriteAllText($mutation.file, $original)
        $text | Set-Content (Join-Path $out "$($mutation.name)-unit.txt") -Encoding utf8
        $intended = $text.Contains("FAIL: $($mutation.expect)")
        $detected = $built -eq 0 -and $code -ne 0 -and $intended
        if (-not $detected) { ++$undetected }
        $log += "$($mutation.name): build=$built 単体試験 exit=$code 意図した検査 '$($mutation.expect)' で失敗=$intended 検出=$detected"
    }
} finally {
    foreach ($file in @($cpp, $header)) { [IO.File]::WriteAllText($file, $originals[$file]) }
}
$restored = $true
foreach ($file in @($cpp, $header)) {
    $after = (Get-FileHash -Algorithm SHA256 $file).Hash
    $restored = $restored -and ($after -eq $before[$file])
    $log += "restored $(Split-Path -Leaf $file) sha256 $after 一致=$($after -eq $before[$file])"
}
pwsh (Join-Path $repo 'scripts\build.ps1') 2>&1 | Out-Null
$rebuilt = $LASTEXITCODE
$text = (& $test 2>&1 | Out-String)
$restoredExit = $LASTEXITCODE
$log += "復元後: build=$rebuilt 単体試験 exit=$restoredExit " + ((($text -split "`n") | Where-Object { $_ -match 'oracle と比べた' }) -join '').Trim()
$log | Set-Content (Join-Path $out 'mutation-log.txt') -Encoding utf8
$log
if ($undetected -ne 0 -or -not $restored -or $restoredExit -ne 0) { exit 1 }
exit 0
