<#
.SYNOPSIS
    P3-1.2: 合成器の source を 1 か所だけ書き換えた変異が、意図した検査で検出されることを確かめる。

.DESCRIPTION
    変異ごとに、source を書き換える -> build -> 単体試験 (失敗を期待し、どの検査で落ちたかを見る)。
    「previous-divide-destination-alpha」は P3-1.2 より前の実装 (下の alpha を 255 で割る) で、
    単体試験の回帰に加えて、実際の MLT との全 frame 比較 (cmp-layers-48) でも不一致へ戻ることを見る。
    最後に必ず source を元の内容へ戻し、SHA-256 の一致を確かめてから build と試験をやり直す。
    作業 tree を書き換えるので、他の build と同時に実行しない。
#>
[CmdletBinding()]
param([string]$OutputDirectory = 'build/math-compositor-p312-20261012')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$source = Join-Path $repo 'src\media\math\math_group_compose.cpp'
$test = Join-Path $repo 'build\ucrt64-release\bin\mvm_test_math_group_compose.exe'
$out = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-mutation')
if (Test-Path -LiteralPath $out) { throw "出力先が既にあります: $out" }
New-Item -ItemType Directory -Force $out | Out-Null
$original = [IO.File]::ReadAllText($source)
$before = (Get-FileHash -Algorithm SHA256 $source).Hash

$mutations = @(
    @{ name = 'previous-divide-destination-alpha'
        find = 'const float below = f32(static_cast<float>(pixel[3]) * f32(1.0f / 255.0f));'
        replace = 'const float below = f32(static_cast<float>(pixel[3]) / 255.0f);'
        expect = '下の alpha が 254 の画素が MLT の観測値と一致する'; mlt = $true
    },
    @{ name = 'alpha-round-instead-of-truncate'
        find = 'pixel[3] = static_cast<std::uint8_t>(f32(255.0f * total));'
        replace = 'pixel[3] = static_cast<std::uint8_t>(f32(255.0f * total) + 0.5f);'
        expect = '整数位置で oracle と全 byte 一致する'; mlt = $false
    },
    @{ name = 'opacity-divide-after-multiply'
        find = 'const float gain = f32(static_cast<float>(layer.opacity) * f32(1.0f / 255.0f));'
        replace = 'const float gain = f32(static_cast<float>(layer.opacity) / 255.0f);'
        expect = '整数位置で oracle と全 byte 一致する'; mlt = $false
    },
    @{ name = 'prepared-roi-one-column-short'
        find = 'raster.roiWidth_ = right - left + 1;'
        replace = 'raster.roiWidth_ = right - left;'
        expect = '字形の範囲は画素から求まる'; mlt = $false
    },
    @{ name = 'destination-alpha-check-removed'
        find = 'if (!mathGroupDestinationEligible(destination, width, height, stride)) {'
        replace = 'if (false) {'
        expect = '不透明でない下地を拒否し、何も書かない'; mlt = $false
    }
)
$log = @("original sha256 $before")
$undetected = 0
try {
    foreach ($mutation in $mutations) {
        if (($original.Split($mutation.find).Count - 1) -ne 1) {
            $log += "$($mutation.name): 変異させる行が 1 か所ではありません (INVALID)"; ++$undetected; continue
        }
        [IO.File]::WriteAllText($source, $original.Replace($mutation.find, $mutation.replace))
        pwsh (Join-Path $repo 'scripts\build.ps1') -Target mvm_test_math_group_compose 2>&1 | Out-Null
        $built = $LASTEXITCODE
        $text = (& $test 2>&1 | Out-String)
        $code = $LASTEXITCODE
        $text | Set-Content (Join-Path $out "$($mutation.name)-unit.txt") -Encoding utf8
        $intended = $text.Contains("FAIL: $($mutation.expect)")
        $line = "$($mutation.name): build=$built 単体試験 exit=$code 意図した検査 '$($mutation.expect)' で失敗=$intended"
        $detected = $built -eq 0 -and $code -ne 0 -and $intended
        if ($mutation.mlt -and $built -eq 0) {
            pwsh (Join-Path $repo 'scripts\build.ps1') -Target mvm_math_score_export_parity 2>&1 | Out-Null
            $harnessBuilt = $LASTEXITCODE
            $rows = & (Join-Path $repo 'scripts\math-compositor-p312-verify.ps1') -OutputDirectory $OutputDirectory `
                -Label "mutation-$($mutation.name)" -Fixtures @('cmp-layers-48') -SkipControls *>&1 | ForEach-Object { "$_" }
            $verifyExit = $LASTEXITCODE
            $summary = ($rows | Where-Object { $_ -match '想定=' }) -join ' '
            $line += " || MLT との比較: build=$harnessBuilt exit=$verifyExit $summary"
            $detected = $detected -and $harnessBuilt -eq 0 -and $verifyExit -ne 0 -and $summary -match '不一致=6 画素=6'
        }
        if (-not $detected) { ++$undetected }
        $log += "$line 検出=$detected"
    }
} finally {
    [IO.File]::WriteAllText($source, $original)
}
$after = (Get-FileHash -Algorithm SHA256 $source).Hash
$log += "restored sha256 $after 一致=$($after -eq $before)"
pwsh (Join-Path $repo 'scripts\build.ps1') -Target mvm_test_math_group_compose 2>&1 | Out-Null
$unitBuilt = $LASTEXITCODE
pwsh (Join-Path $repo 'scripts\build.ps1') -Target mvm_math_score_export_parity 2>&1 | Out-Null
$harnessBuilt = $LASTEXITCODE
$text = (& $test 2>&1 | Out-String)
$restoredExit = $LASTEXITCODE
$log += "復元後: build=$unitBuilt/$harnessBuilt 単体試験 exit=$restoredExit " + ((($text -split "`n") | Where-Object { $_ -match 'oracle と比べた' }) -join '').Trim()
$log | Set-Content (Join-Path $out 'mutation-log.txt') -Encoding utf8
$log
if ($undetected -ne 0 -or $after -ne $before -or $restoredExit -ne 0) { exit 1 }
exit 0
