<#
.SYNOPSIS
    P3-A 専用 deterministic A/V fixture を生成する。
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$ffmpeg = Join-Path $Ucrt64 'bin\ffmpeg.exe'
$ffprobe = Join-Path $Ucrt64 'bin\ffprobe.exe'
$outputDir = Join-Path $repo 'tests\assets\p3_audio'
$output = Join-Path $outputDir 'p3_av_h264_aac.mp4'
$manifest = Join-Path $outputDir 'manifest.json'
if (-not (Test-Path $ffmpeg) -or -not (Test-Path $ffprobe)) {
    throw "UCRT64 の ffmpeg/ffprobe が見つかりません: $Ucrt64"
}
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$duration = 65
$sampleRate = 48000
$markerSamples = @(0, 48000, 240000, 480000, 1440000, 2832000)
$cells = 19
$cellSize = 64
$markerWidth = $cells * $cellSize
$markerExpr = "if(eq(floor(X/$cellSize),0),235," +
    "if(eq(floor(X/$cellSize),1),16," +
    "if(eq(floor(X/$cellSize),18),235," +
    "if(bitand(N,pow(2,floor(X/$cellSize)-2)),235,16))))"
$videoMarker = "nullsrc=s=${markerWidth}x${cellSize}:r=60:d=${duration},format=gray,geq=lum='$markerExpr'"
$drawtext = "drawtext=fontfile='C\:/Windows/Fonts/consola.ttf':text='frame=%{n}  t=%{pts\:hms}':x=8:y=80:fontsize=36:fontcolor=white:box=1:boxcolor=black@0.7"
$videoFilter = "[0:v]$drawtext[b];[1:v]format=yuv420p[m];[b][m]overlay=0:0:shortest=1[v]"

$gates = @($markerSamples | ForEach-Object { "between(n,$_, $($_ + 479))" }) -join '+'
$pulse = "if(gt($gates,0),0.8*sin(2*PI*1000*n/$sampleRate),0)"
$audio = "aevalsrc=exprs='$pulse|$pulse':s=${sampleRate}:d=${duration}:c=stereo"

if ($Force -or -not (Test-Path $output)) {
    & $ffmpeg -hide_banner -y -loglevel error -nostdin `
        -f lavfi -i "testsrc2=s=1920x1080:r=60:d=$duration" `
        -f lavfi -i $videoMarker `
        -f lavfi -i $audio `
        -filter_complex $videoFilter -map '[v]' -map '2:a' `
        -c:v libx264 -preset ultrafast -crf 20 -pix_fmt yuv420p -r 60 -fps_mode cfr -g 60 `
        -c:a aac -b:a 192k -ar $sampleRate -ac 2 -shortest $output
    if ($LASTEXITCODE -ne 0) { throw "P3-A fixture の生成に失敗しました (exit $LASTEXITCODE)" }
}

$probeRaw = & $ffprobe -hide_banner -loglevel error -print_format json `
    -show_format -show_streams -- $output
if ($LASTEXITCODE -ne 0) { throw "P3-A fixture の ffprobe に失敗しました" }
$probe = ($probeRaw -join "`n") | ConvertFrom-Json
$video = $probe.streams | Where-Object codec_type -eq 'video' | Select-Object -First 1
$audioStream = $probe.streams | Where-Object codec_type -eq 'audio' | Select-Object -First 1
if (-not $video -or -not $audioStream -or $video.codec_name -ne 'h264' -or
    [int]$video.width -ne 1920 -or [int]$video.height -ne 1080 -or
    $video.r_frame_rate -ne '60/1' -or $audioStream.codec_name -ne 'aac' -or
    [int]$audioStream.sample_rate -ne 48000 -or [int]$audioStream.channels -ne 2 -or
    [double]$probe.format.duration -lt 65.0) {
    throw '生成済み P3-A fixture が固定 contract と一致しません'
}

$record = [ordered]@{
    schema_version = 1
    file = 'p3_av_h264_aac.mp4'
    sha256 = (Get-FileHash -Algorithm SHA256 $output).Hash.ToLower()
    duration_seconds = [double]$probe.format.duration
    video = [ordered]@{ codec = 'h264'; width = 1920; height = 1080; fps = '60/1'; marker = '19-cell-v1' }
    audio = [ordered]@{
        codec = 'aac'; sample_rate = 48000; channels = 2
        marker_samples = $markerSamples; marker_window_samples = 480
        marker_signal = '左右同相 1000 Hz / amplitude 0.8 / 10 ms'
    }
    ffmpeg_arguments_fixed = $true
}
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifest -Encoding utf8
Write-Host "P3-A fixture: $output" -ForegroundColor Green
Write-Host "manifest      : $manifest"
