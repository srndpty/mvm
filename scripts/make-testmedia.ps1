<#
.SYNOPSIS
    mvm Phase 0 の検証素材を決定論的に生成する (S2)。

.DESCRIPTION
    FFmpeg / ffprobe は必ず MSYS2 UCRT64 版を直接使う。
    ホストには C:\tools と winget 版の FFmpeg も存在するが、それらは使わない。
    mvm がリンクする libav* と同じビルドで素材を作らないと、
    「MLT で読めない」のが素材の問題か MLT の問題か切り分けられなくなる。

    生成した各素材について manifest (JSON) と ffprobe の生 JSON を残す。
    manifest は mvm_bench verify-media が期待値として読む。

    映像は codec 固有の非決定要素 (エンコーダのスレッド分割など) を含むため、
    ファイル hash の完全一致は要求しない。再生成時に照合するのは
    メタデータと期待フレーム数である。

.PARAMETER Mode
    Smoke     : 5 秒。自動検査用。CI とローカルの毎回の実行を想定。
    Benchmark : 60 秒。S7 以降の性能計測用。

.PARAMETER OutputRoot
    出力先。既定は tests/assets/<mode>。git 管理外。

.PARAMETER Force
    既存の素材を再生成する。既定では存在するものはスキップする。

.PARAMETER VerifyRegeneration
    既存 manifest がある場合、再生成してメタデータを照合する。
    hash 不一致は情報として出すが失敗にしない。

.EXAMPLE
    pwsh scripts/make-testmedia.ps1 -Mode Smoke
    pwsh scripts/make-testmedia.ps1 -Mode Benchmark
#>
[CmdletBinding()]
param(
    [ValidateSet('Smoke', 'Benchmark')]
    [string]$Mode = 'Smoke',

    [string]$OutputRoot,
    [switch]$Force,
    [switch]$VerifyRegeneration,
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$FFmpeg   = Join-Path $Ucrt64 'bin\ffmpeg.exe'
$FFprobe  = Join-Path $Ucrt64 'bin\ffprobe.exe'

if (-not $OutputRoot) {
    $OutputRoot = Join-Path $RepoRoot "tests\assets\$($Mode.ToLower())"
}

# --- 使用する FFmpeg を固定する -------------------------------------------
# PATH 上の別 FFmpeg へフォールバックしない。見つからなければ失敗させる。
foreach ($tool in @($FFmpeg, $FFprobe)) {
    if (-not (Test-Path $tool)) {
        throw @"
UCRT64 版の FFmpeg が見つかりません: $tool

PATH 上の別の FFmpeg (C:\tools や winget 版) は使いません。
mvm がリンクする libav* と同じビルドで素材を作る必要があるためです。

    pwsh scripts/bootstrap-msys2.ps1
"@
    }
}

$Duration = if ($Mode -eq 'Smoke') { 5 } else { 60 }
$Fps      = 60
$ExpectedFrames = $Duration * $Fps

Write-Host "=== mvm 検証素材生成 ($Mode) ===" -ForegroundColor Cyan
Write-Host "ffmpeg  : $FFmpeg"
Write-Host "出力先  : $OutputRoot"
Write-Host "尺      : $Duration 秒 / $Fps fps / 期待 $ExpectedFrames フレーム"

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$ProbeDir = Join-Path $OutputRoot '_ffprobe'
New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null

# --- フレーム固有マーカー ---------------------------------------------------
# 仕様は docs/research/test-media-format.md を参照。
#
#   セル幅 64px、帯の高さ 64px、原点 (0,0)。
#   cell 0        : 常に白 (同期)
#   cell 1        : 常に黒 (同期)
#   cell 2..17    : フレーム番号 N の 16bit、LSB first
#   cell 18       : 常に白 (終端同期)
#   合計 19 セル = 1216px
#
# OCR は使わない。セル中心のピクセル値を閾値判定するだけで読める。
$MarkerCells = 19
$CellSize    = 64
$MarkerWidth = $MarkerCells * $CellSize   # 1216
$MarkerExpr  = "if(lt(floor(X/$CellSize),1),235," +
               "if(lt(floor(X/$CellSize),2),16," +
               "if(lt(floor(X/$CellSize),18)," +
               "if(bitand(floor(N/pow(2,floor(X/$CellSize)-2))\,1),235,16)," +
               "235)))"

# 既知の周期を持つ音声。48000Hz に対し整数周期なので完全に決定論的。
#
# 映像に埋める音声 (A1 相当) と WAV (A2 相当) で周波数を変える。
# 同じ構成にすると、片方だけが出力に含まれていても検査が通ってしまい、
# 「両方が mix された」ことを実証できない (S5 の実測で判明)。
#   A1: L 1000Hz (48 サンプル周期) / R  500Hz (96 サンプル周期)
#   A2: L 1500Hz (32 サンプル周期) / R  750Hz (64 サンプル周期)
$AudioExpr = 'aevalsrc=exprs=0.5*sin(2*PI*1000*t)|0.5*sin(2*PI*500*t):s=48000:d=' +
             "${Duration}:c=stereo"
$AudioExprA2 = 'aevalsrc=exprs=0.5*sin(2*PI*1500*t)|0.5*sin(2*PI*750*t):s=48000:d=' +
               "${Duration}:c=stereo"

function New-VideoArgs {
    param(
        [int]$Width, [int]$Height, [string]$VCodec, [string]$PixFmt,
        [string]$Preset, [string]$Output, [switch]$NoAudio,
        # 背景パターン。S5 の合成検証では V1 と V2 が視覚的に区別できる必要がある。
        # 同じ testsrc2 を使うと「全画面で重ねた」のか「縮小して重ねた」のかを
        # 画素から判定できず、検証が空振りする。
        [string]$Pattern = 'testsrc2'
    )

    $marker = "nullsrc=s=${MarkerWidth}x${CellSize}:r=${Fps}:d=${Duration}," +
              "format=gray,geq=lum='$MarkerExpr'"

    # 人間が読めるフレーム番号とタイムコード。マーカー帯の下に置く。
    $drawtext = "drawtext=fontfile='C\:/Windows/Fonts/consola.ttf'" +
                ":text='frame=%{n}  t=%{pts\:hms}':x=8:y=$($CellSize + 16)" +
                ":fontsize=36:fontcolor=white:box=1:boxcolor=black@0.7"

    $filter = "[0:v]$drawtext[b];[1:v]format=yuv420p[m];[b][m]overlay=0:0:shortest=1[v]"

    $a = @(
        '-hide_banner', '-y', '-loglevel', 'error', '-nostdin'
        '-f', 'lavfi', '-i', "${Pattern}=s=${Width}x${Height}:r=${Fps}:d=${Duration}"
        '-f', 'lavfi', '-i', $marker
    )
    if (-not $NoAudio) { $a += @('-f', 'lavfi', '-i', $AudioExpr) }

    $a += @('-filter_complex', $filter, '-map', '[v]')
    if (-not $NoAudio) { $a += @('-map', '2:a', '-c:a', 'aac', '-b:a', '192k', '-ar', '48000', '-ac', '2') }

    $a += @(
        '-c:v', $VCodec, '-preset', $Preset, '-crf', '20'
        '-pix_fmt', $PixFmt, '-r', "$Fps", '-fps_mode', 'cfr', '-g', "$Fps"
        $Output
    )
    return $a
}

# --- 素材定義 ---------------------------------------------------------------
# 4K は素材として重いので preset を落とす。完全な引数は manifest に記録される。

$Assets = @(
    @{
        Id = 'v1080p60_h264'; File = 'v1080p60_h264.mp4'; Kind = 'video'
        Expect = @{ width = 1920; height = 1080; video_codec = 'h264'; pix_fmt = 'yuv420p'
                    fps_num = 60; fps_den = 1; frames = $ExpectedFrames
                    sar_num = 1; sar_den = 1; duration_sec = $Duration
                    audio_codec = 'aac'; sample_rate = 48000; channels = 2; has_alpha = $false }
        Args = { New-VideoArgs -Width 1920 -Height 1080 -VCodec 'libx264' -PixFmt 'yuv420p' `
                               -Preset 'medium' -Output $args[0] }
    },
    @{
        Id = 'v1080p60_hevc'; File = 'v1080p60_hevc.mp4'; Kind = 'video'
        Expect = @{ width = 1920; height = 1080; video_codec = 'hevc'; pix_fmt = 'yuv420p'
                    fps_num = 60; fps_den = 1; frames = $ExpectedFrames
                    sar_num = 1; sar_den = 1; duration_sec = $Duration
                    audio_codec = 'aac'; sample_rate = 48000; channels = 2; has_alpha = $false }
        Args = { New-VideoArgs -Width 1920 -Height 1080 -VCodec 'libx265' -PixFmt 'yuv420p' `
                               -Preset 'medium' -Pattern 'smptehdbars' -Output $args[0] }
    },
    @{
        Id = 'v4k60_h264'; File = 'v4k60_h264.mp4'; Kind = 'video'
        Expect = @{ width = 3840; height = 2160; video_codec = 'h264'; pix_fmt = 'yuv420p'
                    fps_num = 60; fps_den = 1; frames = $ExpectedFrames
                    sar_num = 1; sar_den = 1; duration_sec = $Duration
                    audio_codec = 'aac'; sample_rate = 48000; channels = 2; has_alpha = $false }
        Args = { New-VideoArgs -Width 3840 -Height 2160 -VCodec 'libx264' -PixFmt 'yuv420p' `
                               -Preset 'veryfast' -Output $args[0] }
    },
    @{
        Id = 'v4k60_hevc10'; File = 'v4k60_hevc10.mp4'; Kind = 'video'
        Expect = @{ width = 3840; height = 2160; video_codec = 'hevc'; pix_fmt = 'yuv420p10le'
                    fps_num = 60; fps_den = 1; frames = $ExpectedFrames
                    sar_num = 1; sar_den = 1; duration_sec = $Duration
                    audio_codec = 'aac'; sample_rate = 48000; channels = 2; has_alpha = $false }
        Args = { New-VideoArgs -Width 3840 -Height 2160 -VCodec 'libx265' -PixFmt 'yuv420p10le' `
                               -Preset 'veryfast' -Pattern 'smptehdbars' -Output $args[0] }
    },
    @{
        Id = 'png_alpha'; File = 'png_alpha.png'; Kind = 'image'
        # アルファは (X+Y)*255/(W+H) のグラデーションなので、
        # 透明 (0 付近) と不透明 (253) の両方が必ず含まれる。
        # pix_fmt が rgba でも中身が全て 255 ならアルファは死んでいるので、
        # 実測値域まで検証する。
        Expect = @{ width = 512; height = 512; video_codec = 'png'; pix_fmt = 'rgba'
                    sar_num = 1; sar_den = 1
                    has_alpha = $true; alpha_min_le = 5; alpha_max_ge = 250 }
        Args = {
            # アルファは左上から右下へのグラデーション。
            # 完全不透明・完全透明・中間の全てを含むので、
            # 「アルファが落ちていないか」を数値で判定できる。
            #
            # NOTE: 配列リテラル内で文字列を改行して連結すると、PowerShell は
            #       行末の + を継続と見なさず 2 要素に分割する。必ず 1 行で組む。
            $src = "color=c=black:s=512x512:d=1,format=rgba,geq=r='X*255/W':g='Y*255/H':b='128':a='(X+Y)*255/(W+H)'"
            @(
                '-hide_banner', '-y', '-loglevel', 'error', '-nostdin'
                '-f', 'lavfi', '-i', $src
                '-frames:v', '1', '-c:v', 'png', '-pix_fmt', 'rgba', $args[0]
            )
        }
    },
    @{
        Id = 'wav_48k'; File = 'wav_48k.wav'; Kind = 'audio'
        Expect = @{ audio_codec = 'pcm_s16le'; sample_rate = 48000; channels = 2
                    duration_sec = $Duration; has_alpha = $false }
        Args = {
            @(
                '-hide_banner', '-y', '-loglevel', 'error', '-nostdin'
                '-f', 'lavfi', '-i', $AudioExprA2
                '-c:a', 'pcm_s16le', '-ar', '48000', '-ac', '2', $args[0]
            )
        }
    }
)

# --- 生成 -------------------------------------------------------------------

function Get-Sha256([string]$Path) {
    (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLower()
}

function Invoke-FFprobe([string]$Path) {
    $json = & $FFprobe -hide_banner -loglevel error -print_format json `
                       -show_format -show_streams -- $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ffprobe が失敗しました ($Path):`n$json"
    }
    return ($json -join "`n")
}

$entries = @()

foreach ($asset in $Assets) {
    $outPath = Join-Path $OutputRoot $asset.File

    if ((Test-Path $outPath) -and -not $Force) {
        Write-Host "  skip  $($asset.File) (既存)" -ForegroundColor DarkGray
    } else {
        Write-Host "  生成  $($asset.File) ..." -NoNewline
        $sw = [Diagnostics.Stopwatch]::StartNew()

        $ffArgs = & $asset.Args $outPath
        & $FFmpeg @ffArgs
        if ($LASTEXITCODE -ne 0) {
            throw "ffmpeg が失敗しました ($($asset.Id)), exit $LASTEXITCODE"
        }
        $sw.Stop()
        Write-Host (" {0:N1}s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
    }

    # manifest 用に毎回 probe する (skip した素材も検証対象にする)
    $ffArgs   = & $asset.Args $outPath
    $probeRaw = Invoke-FFprobe $outPath
    $probe    = $probeRaw | ConvertFrom-Json

    $probeFile = Join-Path $ProbeDir "$($asset.Id).ffprobe.json"
    Set-Content -Path $probeFile -Value $probeRaw -Encoding UTF8

    $v = $probe.streams | Where-Object { $_.codec_type -eq 'video' } | Select-Object -First 1
    $a = $probe.streams | Where-Object { $_.codec_type -eq 'audio' } | Select-Object -First 1

    # fps は有理数のまま持つ。小数へ潰すと 60000/1001 のような素材で壊れる。
    $fpsNum = 0; $fpsDen = 0
    if ($v -and $v.PSObject.Properties['r_frame_rate']) {
        $parts = $v.r_frame_rate -split '/'
        $fpsNum = [int]$parts[0]
        $fpsDen = [int]$parts[1]
    }

    $frameCount = 0
    if ($v -and $v.PSObject.Properties['nb_frames'] -and $v.nb_frames) {
        $frameCount = [long]$v.nb_frames
    }

    $entries += [ordered]@{
        id                = $asset.Id
        relative_path     = $asset.File
        kind              = $asset.Kind
        sha256            = Get-Sha256 $outPath
        size_bytes        = (Get-Item $outPath).Length
        container         = $probe.format.format_name
        duration_sec      = if ($probe.format.PSObject.Properties['duration']) { [double]$probe.format.duration } else { 0.0 }
        video_codec       = if ($v) { $v.codec_name } else { '' }
        width             = if ($v) { [int]$v.width } else { 0 }
        height            = if ($v) { [int]$v.height } else { 0 }
        pix_fmt           = if ($v) { $v.pix_fmt } else { '' }
        fps_num           = $fpsNum
        fps_den           = $fpsDen
        frame_count       = $frameCount
        sample_aspect_ratio = if ($v -and $v.PSObject.Properties['sample_aspect_ratio']) { $v.sample_aspect_ratio } else { '' }
        audio_codec       = if ($a) { $a.codec_name } else { '' }
        sample_rate       = if ($a) { [int]$a.sample_rate } else { 0 }
        channels          = if ($a) { [int]$a.channels } else { 0 }
        has_alpha         = [bool]$asset.Expect.has_alpha
        expected          = $asset.Expect
        ffprobe_json      = "_ffprobe/$($asset.Id).ffprobe.json"
        ffmpeg_args       = @($ffArgs)
    }
}

# --- 日本語・空白・全角記号を含むパスへのコピー (V10) -----------------------
# 実際の運用で起きる形に近づける: 日本語ディレクトリ + 全角空白 + 全角記号 +
# 半角空白。ファイル名自体にも日本語を含める。

$JpRelDir = '素材\日本語 テスト\第1回　微分積分＆演習'
$JpDir    = Join-Path $OutputRoot $JpRelDir
New-Item -ItemType Directory -Force -Path $JpDir | Out-Null

$JpCopies = @(
    @{ From = 'v1080p60_h264.mp4'; To = '講義映像　第1回＆演習.mp4' }
    @{ From = 'png_alpha.png';     To = '数式オーバーレイ＆図.png' }
    @{ From = 'wav_48k.wav';       To = 'ナレーション　音声.wav' }
)

$jpEntries = @()
foreach ($c in $JpCopies) {
    $src = Join-Path $OutputRoot $c.From
    $dst = Join-Path $JpDir $c.To
    if (-not (Test-Path $src)) { continue }
    if ((Test-Path $dst) -and -not $Force) {
        # 既存でも manifest には載せる
    } else {
        Copy-Item -Path $src -Destination $dst -Force
    }

    $base = $entries | Where-Object { $_.relative_path -eq $c.From } | Select-Object -First 1
    $clone = [ordered]@{}
    foreach ($k in $base.Keys) { $clone[$k] = $base[$k] }
    $clone['id']            = "$($base.id)_jp"
    $clone['relative_path'] = (Join-Path $JpRelDir $c.To)
    $clone['sha256']        = Get-Sha256 $dst
    $clone['note']          = '日本語・半角空白・全角空白・全角記号を含むパス (V10 検証用)'
    $jpEntries += $clone
}
$entries += $jpEntries

Write-Host "  日本語パスへコピー: $($jpEntries.Count) 件 -> $JpRelDir" -ForegroundColor Green

# --- 破損素材 (negative test 用) --------------------------------------------
# MLT は開けなかった素材でも producer を返す。これらが「成功」扱いされないことを
# 確認するために、意図的に壊れた入力を用意する。
# manifest の assets には載せない (verify-media の検証対象ではない)。

$CorruptDir = Join-Path $OutputRoot '_corrupt'
New-Item -ItemType Directory -Force -Path $CorruptDir | Out-Null

# 1. 0 バイト
$zeroPath = Join-Path $CorruptDir 'zero.mp4'
[System.IO.File]::WriteAllBytes($zeroPath, @())

# 2. ランダムなバイト列 (コンテナとして解釈できない)
$randomPath = Join-Path $CorruptDir 'random.mp4'
$rnd = [byte[]]::new(65536)
# 決定論的にするため固定 seed
$gen = [System.Random]::new(20260804)
$gen.NextBytes($rnd)
[System.IO.File]::WriteAllBytes($randomPath, $rnd)

# 3. 途中で切断された動画 (moov はあるがデータが足りない / あるいは逆)
#    先頭 20% だけを取り出す。fragmented でない mp4 は moov が末尾にあるため、
#    これは「コンテナとして開けない」ケースになる。
$truncPath = Join-Path $CorruptDir 'truncated.mp4'
$srcFull = Join-Path $OutputRoot 'v1080p60_h264.mp4'
if (Test-Path $srcFull) {
    $srcBytes = [System.IO.File]::ReadAllBytes($srcFull)
    $take = [int]($srcBytes.Length * 0.2)
    [System.IO.File]::WriteAllBytes($truncPath, $srcBytes[0..($take - 1)])
}

# 4. 拡張子は動画だが中身がテキスト
$textPath = Join-Path $CorruptDir 'text.mp4'
Set-Content -Path $textPath -Value 'this is not a video file' -Encoding ASCII -NoNewline

# 5. コンテナは正当だが映像も音声も無い (字幕のみの mp4)。
#    破損ではないが「読めるが使えない」入力であり、ユーザーが実際に
#    投入しうる。壊れたバイト列とは別の失敗経路を通る。
$subOnlyPath = Join-Path $CorruptDir 'subtitle_only.mp4'
$srtPath = Join-Path $CorruptDir '_sub.srt'
Set-Content -Path $srtPath -Value "1`r`n00:00:00,000 --> 00:00:03,000`r`nmvm phase0`r`n" -Encoding ASCII
& $FFmpeg -hide_banner -loglevel error -y -i $srtPath -c:s mov_text $subOnlyPath
if ($LASTEXITCODE -ne 0) { Write-Host '  (字幕のみ mp4 の生成に失敗しました)' -ForegroundColor Yellow }

Write-Host "  破損・退化素材 (negative test 用): 5 件 -> _corrupt\" -ForegroundColor Green

# --- manifest ---------------------------------------------------------------

$ffVersion = (& $FFmpeg -hide_banner -version 2>&1 | Select-Object -First 1)

$manifest = [ordered]@{
    schema_version   = 1
    mode             = $Mode
    generated_at     = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
    duration_sec     = $Duration
    fps              = $Fps
    expected_frames  = $ExpectedFrames
    ffmpeg           = $FFmpeg
    ffmpeg_version   = $ffVersion
    marker = [ordered]@{
        cell_size_px  = $CellSize
        cell_count    = $MarkerCells
        band_width_px = $MarkerWidth
        band_height_px= $CellSize
        origin_x      = 0
        origin_y      = 0
        white_level   = 235
        black_level   = 16
        layout        = 'cell0=sync_white, cell1=sync_black, cell2..17=frame index 16bit LSB-first, cell18=sync_white'
        spec_doc      = 'docs/research/test-media-format.md'
    }
    audio = [ordered]@{
        left_hz     = 1000
        right_hz    = 500
        sample_rate = 48000
        note        = '48000Hz に対し整数周期 (48 / 96 サンプル)。位相は t=0 で 0。'
    }
    assets = $entries
}

$manifestPath = Join-Path $OutputRoot 'manifest.json'

# --- 再生成時のメタデータ照合 ----------------------------------------------
if ($VerifyRegeneration -and (Test-Path $manifestPath)) {
    Write-Host "`n--- 再生成の照合 ---" -ForegroundColor Yellow
    $old = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json

    $metaFields = @('container','video_codec','width','height','pix_fmt',
                    'fps_num','fps_den','frame_count','audio_codec','sample_rate','channels')
    $diffs = 0; $hashDiffs = 0

    foreach ($new in $entries) {
        $prev = $old.assets | Where-Object { $_.id -eq $new.id } | Select-Object -First 1
        if (-not $prev) { continue }
        foreach ($f in $metaFields) {
            if ("$($prev.$f)" -ne "$($new[$f])") {
                Write-Host "  META 不一致 $($new.id).$f : '$($prev.$f)' -> '$($new[$f])'" -ForegroundColor Red
                $diffs++
            }
        }
        if ($prev.sha256 -ne $new['sha256']) { $hashDiffs++ }
    }

    Write-Host "  メタデータ不一致 : $diffs 件"
    Write-Host "  hash 不一致       : $hashDiffs 件 (映像 codec の非決定性により発生しうる。失敗ではない)"
    if ($diffs -gt 0) {
        throw "再生成でメタデータが変化しました。生成条件が決定論的ではありません。"
    }
}

$manifest | ConvertTo-Json -Depth 8 | Set-Content -Path $manifestPath -Encoding UTF8

Write-Host "`nmanifest: $manifestPath" -ForegroundColor Cyan
Write-Host "素材数  : $($entries.Count)"
Write-Host "`n次: mvm_bench verify-media `"$manifestPath`""
