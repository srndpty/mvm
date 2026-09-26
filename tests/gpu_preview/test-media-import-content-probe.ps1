[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Bench,
    [Parameter(Mandatory)][string]$AudioSeekTest,
    [Parameter(Mandatory)][string]$MpgExportTest,
    [Parameter(Mandatory)][string]$FFmpeg,
    [Parameter(Mandatory)][string]$WorkDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-That([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-MediaProbe([string]$MediaPath, [string]$JsonPath) {
    $diagnosticPath = "$JsonPath.stderr.txt"
    & $Bench probe $MediaPath --json $JsonPath 2> $diagnosticPath
    $exitCode = $LASTEXITCODE
    Assert-That ($exitCode -in @(0, 3)) "probe が予期しない終了コードで失敗しました: $exitCode"
    Assert-That (Test-Path -LiteralPath $JsonPath -PathType Leaf) "probe JSON がありません: $JsonPath"
    return Get-Content -LiteralPath $JsonPath -Raw -Encoding utf8 | ConvertFrom-Json
}

$benchPath = [IO.Path]::GetFullPath($Bench)
$audioSeekTestPath = [IO.Path]::GetFullPath($AudioSeekTest)
$mpgExportTestPath = [IO.Path]::GetFullPath($MpgExportTest)
$ffmpegPath = [IO.Path]::GetFullPath($FFmpeg)
$workPath = [IO.Path]::GetFullPath($WorkDirectory)
Assert-That (Test-Path -LiteralPath $benchPath -PathType Leaf) "mvm_bench がありません: $benchPath"
Assert-That (Test-Path -LiteralPath $audioSeekTestPath -PathType Leaf) `
    "audio seek testがありません: $audioSeekTestPath"
Assert-That (Test-Path -LiteralPath $mpgExportTestPath -PathType Leaf) `
    "MPG export testがありません: $mpgExportTestPath"
Assert-That (Test-Path -LiteralPath $ffmpegPath -PathType Leaf) "UCRT64 FFmpeg がありません: $ffmpegPath"
New-Item -ItemType Directory -Path $workPath -Force | Out-Null

$mpgPath = Join-Path $workPath 'mpeg2-with-mp2.mpg'
& $ffmpegPath -hide_banner -loglevel error -y `
    -f lavfi -i 'testsrc2=size=320x240:rate=30000/1001' `
    -f lavfi -i 'sine=frequency=1000:sample_rate=48000' `
    -t 1 -c:v mpeg2video -pix_fmt yuv420p -c:a mp2 -f mpeg $mpgPath
Assert-That ($LASTEXITCODE -eq 0) "MPG fixture を生成できません: exit $LASTEXITCODE"

$mpgProbe = Invoke-MediaProbe $mpgPath (Join-Path $workPath 'mpg-probe.json')
Assert-That ($mpgProbe.mlt_ok -eq $true) 'MLT が MPG を解析できませんでした'
Assert-That ($mpgProbe.mlt.has_video -eq $true) 'MPG の映像 stream を検出できませんでした'
Assert-That ($mpgProbe.mlt.has_audio -eq $true) 'MPG の音声 stream を検出できませんでした'
Assert-That ($mpgProbe.mlt.video_codec -eq 'mpeg2video') "MPG の映像 codec が違います: $($mpgProbe.mlt.video_codec)"
Assert-That ($mpgProbe.mlt.audio_codec -eq 'mp2') "MPG の音声 codec が違います: $($mpgProbe.mlt.audio_codec)"
Assert-That ($mpgProbe.mlt.is_unbounded_length -eq $false) 'MPG が有限尺として認識されませんでした'
Assert-That ([long]$mpgProbe.mlt.frame_count -gt 0) 'MPG の frame count が正ではありません'
Assert-That ([long]$mpgProbe.mlt.fps_num -gt 0 -and [long]$mpgProbe.mlt.fps_den -gt 0) `
    'MPG の FPS が有効ではありません'

# MPEG-PSは非ゼロのstream開始PTSを持つ。このfixtureでsample 0と途中位置の
# exact audio seekを満たし、絶対PTSをそのままsample番号にしないことを固定する。
& $audioSeekTestPath $mpgPath
Assert-That ($LASTEXITCODE -eq 0) "非ゼロ開始PTSのaudio seekに失敗しました: exit $LASTEXITCODE"

$mpgExportPath = Join-Path $workPath 'mpeg2-export.mp4'
& $mpgExportTestPath $mpgPath $mpgExportPath
Assert-That ($LASTEXITCODE -eq 0) "音声付きMPGの書き出しに失敗しました: exit $LASTEXITCODE"

# 拡張子を変えても同じ内容なら受理できることを固定する。
$unknownExtensionPath = Join-Path $workPath 'mpeg2-content.unknown-video'
Copy-Item -LiteralPath $mpgPath -Destination $unknownExtensionPath -Force
$unknownProbe = Invoke-MediaProbe $unknownExtensionPath (Join-Path $workPath 'unknown-extension-probe.json')
Assert-That ($unknownProbe.mlt_ok -eq $true -and $unknownProbe.mlt.has_video -eq $true) `
    '拡張子を持たない同一内容を解析できませんでした'

# 拡張子だけ MPG の非メディアを通さない negative test。
$fakeMpgPath = Join-Path $workPath 'not-media.mpg'
Set-Content -LiteralPath $fakeMpgPath -Value 'これは動画ではありません' -Encoding utf8
$fakeProbe = Invoke-MediaProbe $fakeMpgPath (Join-Path $workPath 'fake-mpg-probe.json')
Assert-That ($fakeProbe.mlt_ok -eq $false) '拡張子だけ MPG の非メディアを受理しました'

Write-Host 'media import content probe: PASS'
