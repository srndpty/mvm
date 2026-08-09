<#
.SYNOPSIS
    4K 素材から proxy を生成し、生成速度・サイズ・メタデータを記録する (S7 / M8)。

.DESCRIPTION
    候補:
      gop12  : 960x540 H.264 NVENC、GOP 12
      gop1   : 960x540 H.264 NVENC、GOP 1 (all-intra)
      x264   : 960x540 libx264、GOP 12 (NVENC 不在時の診断用。正式候補にしない)

    音声は再エンコードせず -c:a copy でそのまま通す。

    [事実] AAC を再エンコードすると音声ストリームが 60.000s -> 60.010s に伸び、
    MLT が見る尺が 3600 frame から 3601 frame に変わった。
    映像ストリームは 3600 frame / 60.000s のままである。
    ffprobe の nb_frames は両方 3600 なので、映像だけ見ていると気づかない。
    proxy を切り替えるだけでタイムラインの長さが変わることになる。

    生成は必ず一時ファイル (.mvmtmp) へ行い、ffprobe による検証に成功してから
    正規名へ rename する。失敗した部分ファイルを成果物として残さない。

    FFmpeg は C:\msys64\ucrt64\bin のものだけを使う。
    ホストの C:\tools 版や winget 版へフォールバックしない。

    M8 の生成速度: realtime_ratio = source_duration / generation_wall_time >= 2.0

.EXAMPLE
    pwsh scripts/proxy-matrix.ps1
    pwsh scripts/proxy-matrix.ps1 -Candidates gop12
#>
[CmdletBinding()]
param(
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [string]$Source,
    [string]$Tag = 'v4k60_h264',
    [string[]]$Candidates = @('gop12', 'gop1', 'x264'),
    [string]$OutDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$FFmpeg   = Join-Path $Ucrt64 'bin\ffmpeg.exe'
$FFprobe  = Join-Path $Ucrt64 'bin\ffprobe.exe'
$Bench    = Join-Path $RepoRoot 'tests\assets\benchmark'
if (-not $Source) { $Source = Join-Path $Bench 'v4k60_h264.mp4' }
if (-not $OutDir) { $OutDir = Join-Path $Bench '_proxy' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($t in @($FFmpeg, $FFprobe)) {
    if (-not (Test-Path $t)) { throw "UCRT64 の FFmpeg がありません: $t" }
}
if (-not (Test-Path $Source)) { throw "元素材がありません: $Source" }

function Get-MediaInfo {
    param([string]$Path)
    $json = & $FFprobe -v error -print_format json -show_format -show_streams $Path
    if ($LASTEXITCODE -ne 0) { throw "ffprobe が失敗しました: $Path" }
    $o = ($json | Out-String) | ConvertFrom-Json
    $v = $o.streams | Where-Object { $_.codec_type -eq 'video' } | Select-Object -First 1
    $a = $o.streams | Where-Object { $_.codec_type -eq 'audio' } | Select-Object -First 1
    if (-not $v) { throw "映像ストリームがありません: $Path" }
    [pscustomobject]@{
        Width      = [int]$v.width
        Height     = [int]$v.height
        RFrameRate = [string]$v.r_frame_rate
        NbFrames   = if ($v.PSObject.Properties['nb_frames']) { [long]$v.nb_frames } else { -1 }
        Duration   = [double]$o.format.duration
        Sar        = if ($v.PSObject.Properties['sample_aspect_ratio']) { [string]$v.sample_aspect_ratio } else { 'unset' }
        PixFmt     = [string]$v.pix_fmt
        Codec      = [string]$v.codec_name
        Profile    = if ($v.PSObject.Properties['profile']) { [string]$v.profile } else { '' }
        AudioRate  = if ($a) { [int]$a.sample_rate } else { 0 }
        AudioCh    = if ($a) { [int]$a.channels } else { 0 }
        AudioCodec = if ($a) { [string]$a.codec_name } else { '' }
        SizeBytes  = [long]$o.format.size
    }
}

# keyframe 間隔を実測する。指定した -g がそのまま出力に効いているとは限らない。
function Get-KeyframeInterval {
    param([string]$Path, [int]$Limit = 600)
    $lines = & $FFprobe -v error -select_streams v:0 -show_entries frame=key_frame `
        -read_intervals "%+#$Limit" -of csv=p=0 $Path
    $idx = @()
    $i = 0
    foreach ($l in $lines) {
        if ("$l".Trim() -eq '1') { $idx += $i }
        $i++
    }
    if ($idx.Count -lt 2) {
        return [pscustomobject]@{ Count = $idx.Count; MinGap = -1; MaxGap = -1; Frames = $i }
    }
    $gaps = @()
    for ($k = 1; $k -lt $idx.Count; $k++) { $gaps += ($idx[$k] - $idx[$k - 1]) }
    [pscustomobject]@{
        Count  = $idx.Count
        MinGap = ($gaps | Measure-Object -Minimum).Minimum
        MaxGap = ($gaps | Measure-Object -Maximum).Maximum
        Frames = $i
    }
}

$src = Get-MediaInfo -Path $Source
Write-Host "=== 元素材 ===" -ForegroundColor Cyan
Write-Host ("{0}x{1} {2} {3} frames {4:N3}s {5} {6}" -f `
    $src.Width, $src.Height, $src.RFrameRate, $src.NbFrames, $src.Duration, $src.Codec, $src.PixFmt)

# 候補ごとの encoder 設定。実際に渡す option をそのまま記録する。
$specs = @{
    'gop12' = @{
        Encoder = 'h264_nvenc'
        Gop     = 12
        Options = @('-c:v', 'h264_nvenc', '-preset', 'p4', '-rc', 'vbr', '-cq', '25',
                    '-b:v', '0', '-g', '12', '-bf', '0')
        Formal  = $true
    }
    # [事実] h264_nvenc は GOP 長 1 を受け付けない。
    #   InitializeEncoder failed: invalid param (8):
    #   Gop Length should be greater than number of B frames + 1
    # -bf 0 にしても条件は 1 > 1 で偽になるため通らない。-g 2 は通る。
    # したがって all-intra proxy は libx264 で作るしかない。
    #
    # **encoder が違うので gop1 と gop12 の差は GOP だけの差ではない。**
    # 切り分けのために x264-gop12 (同じ encoder, 同じ GOP) を対照として残す。
    #   gop1 vs x264      -> GOP の効果 (encoder は同じ)
    #   x264 vs gop12     -> encoder の効果 (GOP は同じ)
    'gop1'  = @{
        Encoder = 'libx264'
        Gop     = 1
        Options = @('-c:v', 'libx264', '-preset', 'veryfast', '-crf', '23',
                    '-g', '1', '-bf', '0')
        Formal  = $true
    }
    'x264'  = @{
        Encoder = 'libx264'
        Gop     = 12
        Options = @('-c:v', 'libx264', '-preset', 'veryfast', '-crf', '23',
                    '-g', '12', '-bf', '0')
        # gop1 と gop12 の差から encoder の効果を切り分けるための対照。
        # NVENC 不在時の fallback 診断も兼ねる。正式な proxy 候補にはしない。
        Formal  = $false
    }
}

$rows = @()
foreach ($cand in $Candidates) {
    if (-not $specs.ContainsKey($cand)) { throw "未知の候補: $cand" }
    $spec = $specs[$cand]
    $final = Join-Path $OutDir "${Tag}_proxy_$cand.mp4"
    $tmp   = "$final.mvmtmp"

    if ((Test-Path $final) -and -not $Force) {
        Write-Host "`n=== $cand : 既存の成果物を再利用します ===" -ForegroundColor Yellow
        $info = Get-MediaInfo -Path $final
        $kf = Get-KeyframeInterval -Path $final
        $rows += [pscustomobject]([ordered]@{
            Candidate = $cand; Encoder = $spec.Encoder; RequestedGop = $spec.Gop
            Formal = $spec.Formal; Regenerated = $false
            Width = $info.Width; Height = $info.Height; Fps = $info.RFrameRate
            Frames = $info.NbFrames; DurationSec = [math]::Round($info.Duration, 3)
            Sar = $info.Sar; PixFmt = $info.PixFmt; Codec = $info.Codec
            AudioRate = $info.AudioRate; AudioCh = $info.AudioCh; AudioCodec = $info.AudioCodec
            KeyframeMinGap = $kf.MinGap; KeyframeMaxGap = $kf.MaxGap
            SizeMb = [math]::Round($info.SizeBytes / 1MB, 1)
            GenWallSec = -1.0; RealtimeRatio = -1.0
            Options = ($spec.Options -join ' ')
            M8Speed = '(再利用のため未測定)'
        })
        continue
    }

    Write-Host "`n=== $cand ($($spec.Encoder), GOP $($spec.Gop)) ===" -ForegroundColor Cyan
    Remove-Item $tmp -ErrorAction SilentlyContinue

    # 一時ファイルの拡張子は .mvmtmp なので、ffmpeg は拡張子から
    # コンテナを推測できない。-f mp4 を明示する。
    # 明示しないと「Unable to choose an output format」で失敗する。
    $ffmpegArgs = @('-hide_banner', '-v', 'error', '-y', '-i', $Source,
              '-vf', 'scale=960:540') + $spec.Options + @(
              '-c:a', 'copy', '-movflags', '+faststart',
              '-f', 'mp4', $tmp)

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $FFmpeg @ffmpegArgs
    $enc = $LASTEXITCODE
    $sw.Stop()
    $wall = $sw.Elapsed.TotalSeconds

    if ($enc -ne 0) {
        Remove-Item $tmp -ErrorAction SilentlyContinue
        Write-Host "$cand : ffmpeg が exit $enc で失敗しました。部分ファイルは削除しました。" -ForegroundColor Red
        if ($spec.Formal) { throw "正式候補 $cand の生成に失敗しました" }
        continue
    }

    # 検証してから rename する。壊れた出力を成果物として残さない。
    $ok = $true
    $why = ''
    try {
        $info = Get-MediaInfo -Path $tmp
        if ($info.Width -ne 960 -or $info.Height -ne 540) { $ok = $false; $why = "解像度が違います ($($info.Width)x$($info.Height))" }
        elseif ($info.RFrameRate -ne $src.RFrameRate)     { $ok = $false; $why = "fps が元と違います ($($info.RFrameRate) vs $($src.RFrameRate))" }
        elseif ($info.NbFrames -ne $src.NbFrames)         { $ok = $false; $why = "frame 数が元と違います ($($info.NbFrames) vs $($src.NbFrames))" }
        # duration は「1 frame 未満」を要求する。ちょうど 1 frame は通さない。
        # 音声を再エンコードすると 60.000 -> 60.010 になり、MLT が見る尺が
        # 3600 -> 3601 frame に変わる。ffprobe の nb_frames は 3600 のままなので
        # 映像だけ見ていると気づかない。
        elseif ([math]::Abs($info.Duration - $src.Duration) -ge (1.0 / 60.0)) { $ok = $false; $why = "duration の差が 1 frame 以上あります ($($info.Duration) vs $($src.Duration))" }
        elseif ($info.AudioRate -ne 48000)                { $ok = $false; $why = "音声 sample rate が 48000 ではありません ($($info.AudioRate))" }
    } catch {
        $ok = $false; $why = "$_"
    }

    if (-not $ok) {
        Remove-Item $tmp -ErrorAction SilentlyContinue
        Write-Host "$cand : 検証に失敗したので破棄しました: $why" -ForegroundColor Red
        if ($spec.Formal) { throw "正式候補 $cand の検証に失敗しました: $why" }
        continue
    }

    Move-Item -Force $tmp $final
    $info = Get-MediaInfo -Path $final
    $kf = Get-KeyframeInterval -Path $final
    $ratio = if ($wall -gt 0) { $src.Duration / $wall } else { 0 }

    $rows += [pscustomobject]([ordered]@{
        Candidate = $cand; Encoder = $spec.Encoder; RequestedGop = $spec.Gop
        Formal = $spec.Formal; Regenerated = $true
        Width = $info.Width; Height = $info.Height; Fps = $info.RFrameRate
        Frames = $info.NbFrames; DurationSec = [math]::Round($info.Duration, 3)
        Sar = $info.Sar; PixFmt = $info.PixFmt; Codec = $info.Codec
        AudioRate = $info.AudioRate; AudioCh = $info.AudioCh; AudioCodec = $info.AudioCodec
        KeyframeMinGap = $kf.MinGap; KeyframeMaxGap = $kf.MaxGap
        SizeMb = [math]::Round($info.SizeBytes / 1MB, 1)
        GenWallSec = [math]::Round($wall, 2)
        RealtimeRatio = [math]::Round($ratio, 2)
        Options = ($spec.Options -join ' ')
        M8Speed = if ($ratio -ge 2.0) { '合格' } else { '不合格' }
    })
    Write-Host ("  {0:N1}s で生成 (realtime 比 {1:N2}x), {2:N1} MB, keyframe 間隔 {3}..{4}" -f `
        $wall, $ratio, ($info.SizeBytes / 1MB), $kf.MinGap, $kf.MaxGap)
}

Write-Host "`n=== proxy 候補 ===" -ForegroundColor Cyan
$rows | Format-Table Candidate, Encoder, RequestedGop, Formal, Width, Height, Fps, Frames,
                     DurationSec, Sar, PixFmt, AudioRate, KeyframeMinGap, KeyframeMaxGap,
                     SizeMb, GenWallSec, RealtimeRatio, M8Speed -AutoSize

$rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir "proxy-matrix-$Tag.json") -Encoding UTF8
Write-Host "成果物: $OutDir"
