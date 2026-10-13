<#
.SYNOPSIS
    P3-5A.2: V1 の隙間を含む書き出しを、実物の書き出しの関数 (Windows の製品の binary と同じ code) で
    くり返し、出力の MP4 と encoder 直前の RGBA を記録する。修正の前後で同じものを実行して比べる。

.DESCRIPTION
    mvm_test_mlt_export_v1_gap.exe の export mode を使う。scenario (gap・mixed・multi) ×
    RGBA の検査あり・なし × worker 1・8 本。worker 8 本は -Repeats 回くり返す。
    1 回ごとに、終了コード、出力の MP4 の SHA-256、(検査ありなら) frame ごとの RGBA の digest の
    SHA-256、隙間だけの frame のうち黒でなかった数、を summary.json へ書く。build はしない。

    -Compare に、比べる相手の run の directory を渡すと、同じ条件どうしの MP4 と RGBA の digest が
    一致するかを調べる (修正の後の run で、修正の前の run を渡す)。
#>
param(
    [Parameter(Mandatory)][string]$Label,
    [string]$OutputDirectory = 'build/mlt-color-race-p35a2-20261013',
    [int]$Repeats = 5,
    [int]$Fault = 0,
    [string]$Compare = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\ucrt64-release\bin\mvm_test_mlt_export_v1_gap.exe'
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$run = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Label")
if (Test-Path -LiteralPath $run) { throw "出力先が既にあります: $run" }
New-Item -ItemType Directory -Force $run | Out-Null

$hashed = @($exe, 'C:\msys64\ucrt64\bin\libmlt-7.dll', 'C:\msys64\ucrt64\lib\mlt\libmltcore.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltplus.dll', 'C:\msys64\ucrt64\lib\mlt\libmltavformat.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltqt6.dll', 'C:\msys64\ucrt64\bin\libx264-165.dll',
    (Join-Path $repo 'src/media/mlt/mvm_mlt_export.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_export.h'),
    (Join-Path $repo 'src/media/mlt/mvm_mlt_v1_background.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_v1_background.h'),
    (Join-Path $repo 'tests/harness/test_mlt_export_v1_gap.cpp'), (Join-Path $repo 'tests/assets/smoke/png_alpha.png'),
    (Join-Path $repo 'scripts/mlt-color-race-p35a2-windows.ps1'))
$identity = [ordered]@{ label = $Label; fault = $Fault; git_head = (git -C $repo rev-parse HEAD)
    git_status = @(git -C $repo status --porcelain); sha256 = [ordered]@{}
}
foreach ($file in $hashed) { $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash }
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'identity.json') -Encoding utf8
(git -C $repo diff HEAD) | Set-Content -LiteralPath (Join-Path $run 'git-diff.patch') -Encoding utf8

# 隙間だけの frame (scenario ごと。試験の source と同じ)。
$gapOnly = @{
    gap   = @(0..119)
    mixed = @(0..29) + @(90..119)
    multi = @(0..19) + @(100..119)
}
$rows = @()
foreach ($scenario in @('gap', 'mixed', 'multi')) {
    foreach ($validator in @(1, 0)) {
        foreach ($workers in @(1, 8)) {
            $count = if ($workers -eq 8) { $Repeats } else { 1 }
            for ($index = 1; $index -le $count; ++$index) {
                $name = "$scenario-v$validator-w$workers-$index"
                $mp4 = Join-Path $run "$name.mp4"
                $tsv = Join-Path $run "$name.tsv"
                & $exe export $scenario $validator $workers $mp4 $Fault > $tsv 2> (Join-Path $run "$name.log")
                $code = $LASTEXITCODE
                $notBlack = $null; $missing = $null; $digest = $null
                if ($validator -eq 1 -and $code -eq 0) {
                    $lines = @(Get-Content -LiteralPath $tsv)
                    $notBlack = 0; $missing = 0
                    foreach ($line in $lines) {
                        $c = $line -split "`t"
                        if ([int]$c[1] -eq 0) { ++$missing }
                        if ([int]$c[0] -in $gapOnly[$scenario] -and $c[3] -ne '1') { ++$notBlack }
                    }
                    # 呼ばれた回数は比べない (最後の frame は何度も要求される)。frame と画素の digest だけ。
                    $text = ($lines | ForEach-Object { $c = $_ -split "`t"; "$($c[0]) $($c[2])" }) -join "`n"
                    $digest = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text))).Replace('-', '')
                }
                $rows += [pscustomobject]@{ name = $name; scenario = $scenario; validator = $validator; workers = $workers; index = $index
                    exit_code = $code; mp4_sha256 = if (Test-Path -LiteralPath $mp4) { (Get-FileHash -Algorithm SHA256 -LiteralPath $mp4).Hash } else { $null }
                    rgba_digest_sha256 = $digest; gap_frames_not_black = $notBlack; frames_missing = $missing
                    log = ((Get-Content -LiteralPath (Join-Path $run "$name.log") | Select-Object -Last 1) -join '')
                }
                Write-Host ("{0,-20} exit={1} mp4={2} rgba={3} 黒でない隙間={4} 欠け={5}" -f $name, $code,
                    ($rows[-1].mp4_sha256 ?? '-').Substring(0, [math]::Min(12, ($rows[-1].mp4_sha256 ?? '-').Length)),
                    ($digest ?? '-').Substring(0, [math]::Min(12, ($digest ?? '-').Length)), $notBlack, $missing)
            }
        }
    }
}
$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8

# 同じ条件 (scenario・検査の有無) の中で、worker の数・くり返しによらず出力が 1 種類か。
$failures = 0
foreach ($group in ($rows | Group-Object scenario, validator)) {
    $mp4 = @($group.Group | ForEach-Object { $_.mp4_sha256 } | Sort-Object -Unique)
    $rgba = @($group.Group | Where-Object { $_.validator -eq 1 } | ForEach-Object { $_.rgba_digest_sha256 } | Sort-Object -Unique)
    $bad = @($group.Group | Where-Object { $_.exit_code -ne 0 -or ($_.gap_frames_not_black -gt 0) -or ($_.frames_missing -gt 0) }).Count
    Write-Host ("{0}: MP4 {1} 種類、RGBA {2} 種類、失敗・黒でない・欠け {3} 件 ({4} run)" -f $group.Name, $mp4.Count, $rgba.Count, $bad, $group.Count)
    if ($mp4.Count -ne 1 -or $rgba.Count -gt 1 -or $bad -gt 0) { ++$failures }
}
if ($Compare) {
    $other = Get-Content -Raw (Join-Path (Join-Path $repo $Compare) 'summary.json') | ConvertFrom-Json
    $same = 0; $different = 0
    foreach ($row in $rows) {
        $match = @($other | Where-Object { $_.name -eq $row.name })
        if ($match.Count -ne 1) { continue }
        if ($match[0].mp4_sha256 -eq $row.mp4_sha256 -and $match[0].rgba_digest_sha256 -eq $row.rgba_digest_sha256) { ++$same }
        else { ++$different; Write-Host "  違う: $($row.name)" }
    }
    Write-Host "比較 ($Compare): 一致 $same、不一致 $different"
    if ($different -gt 0 -or $same -eq 0) { ++$failures }
}
Write-Host "結果: $run"
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
