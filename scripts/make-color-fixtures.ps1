<#
.SYNOPSIS
    color correctness 検査用の決定論的 fixture を生成する (P1.1 §6)。

.DESCRIPTION
    marker 一致は color correctness の証拠にならない。
    marker は白 235 / 黒 16 の高コントラストなので、
    変換係数がずれていても読めてしまう。

    そこで **既知の YUV 値** を持つ patch を焼き込んだ素材を作り、
    表示と同じ shader で RGB 化した結果を期待値と照合する。

    期待 RGB はこのスクリプトが **標準式から独立に**計算し、manifest へ書く。
    実装 (coefficientsFor) を呼んで期待値を作ると、実装のバグを
    テストが追認してしまう。

    レイアウト:
      512 x 256 の映像。上端 64 行に 64x64 の patch を 8 個並べる。
      残りは中間グレーで埋める (hw decoder の最小サイズ制約を避けるため
      縦を 256 にしている)。

    生成物:
      tests/assets/color/<id>.mp4
      tests/assets/color/manifest.json

.PARAMETER OutDir
    出力先。既定は tests/assets/color。

.EXAMPLE
    pwsh scripts/make-color-fixtures.ps1
#>
[CmdletBinding()]
param(
    [string]$OutDir,
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'tests\assets\color' }

# FFmpeg は UCRT64 版だけを使う。ホストの別 FFmpeg へフォールバックしない
# (Phase 0 からの方針)。
$FFmpeg = Join-Path $Ucrt64 'bin\ffmpeg.exe'
if (-not (Test-Path $FFmpeg)) { throw "UCRT64 の ffmpeg が見つかりません: $FFmpeg" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Width = 512
$Height = 256
$PatchSize = 64
$PatchCount = 8

# patch の元になる RGB (0..255)。原色と中間色を混ぜる。
# 原色だけだと chroma の符号ミスを見逃す組み合わせがある。
$patchRgb = @(
    @(255, 255, 255),  # white
    @(0, 0, 0),        # black
    @(128, 128, 128),  # mid gray
    @(255, 0, 0),      # red
    @(0, 255, 0),      # green
    @(0, 0, 255),      # blue
    @(0, 255, 255),    # cyan
    @(255, 255, 0)     # yellow
)

# 行列は Kr / Kb で決まる。式を 3 回書かない。
$matrices = @{
    'bt709'     = @{ kr = 0.2126;  kb = 0.0722  }
    'bt601'     = @{ kr = 0.299;   kb = 0.114   }
    'bt2020ncl' = @{ kr = 0.2627;  kb = 0.0593  }
}

function Clamp([double]$v, [double]$lo, [double]$hi) {
    if ($v -lt $lo) { return $lo }
    if ($v -gt $hi) { return $hi }
    return $v
}

# RGB -> 量子化 YUV。depth は 8 か 10。
function ConvertTo-Yuv([int[]]$rgb, [double]$kr, [double]$kb, [string]$range, [int]$depth) {
    $kg = 1.0 - $kr - $kb
    $r = $rgb[0] / 255.0
    $g = $rgb[1] / 255.0
    $b = $rgb[2] / 255.0
    $y = $kr * $r + $kg * $g + $kb * $b
    $cb = 0.5 * ($b - $y) / (1.0 - $kb)
    $cr = 0.5 * ($r - $y) / (1.0 - $kr)

    $maxv = [math]::Pow(2, $depth) - 1
    $scale = [math]::Pow(2, $depth - 8)
    if ($range -eq 'full') {
        $yq = [math]::Round($y * $maxv)
        $uq = [math]::Round(($cb + 0.5) * $maxv)
        $vq = [math]::Round(($cr + 0.5) * $maxv)
    } else {
        $yq = [math]::Round((16 + 219 * $y) * $scale)
        $uq = [math]::Round((128 + 224 * $cb) * $scale)
        $vq = [math]::Round((128 + 224 * $cr) * $scale)
    }
    return @([int](Clamp $yq 0 $maxv), [int](Clamp $uq 0 $maxv), [int](Clamp $vq 0 $maxv))
}

# 量子化 YUV -> 期待 RGB。**標準式をそのまま書く。**
# 実装側の関数を呼ばない (実装のバグを追認しないため)。
function ConvertTo-ExpectedRgb([int[]]$yuv, [double]$kr, [double]$kb, [string]$range, [int]$depth) {
    $kg = 1.0 - $kr - $kb
    $maxv = [math]::Pow(2, $depth) - 1
    $scale = [math]::Pow(2, $depth - 8)
    $neutral = 128 * $scale
    if ($range -eq 'full') {
        $y = $yuv[0] / $maxv
        $cb = ($yuv[1] - $neutral) / $maxv
        $cr = ($yuv[2] - $neutral) / $maxv
    } else {
        $y = ($yuv[0] - 16 * $scale) / (219 * $scale)
        $cb = ($yuv[1] - $neutral) / (224 * $scale)
        $cr = ($yuv[2] - $neutral) / (224 * $scale)
    }
    $r = $y + 2 * (1 - $kr) * $cr
    $b = $y + 2 * (1 - $kb) * $cb
    $g = $y - (2 * (1 - $kb) * $kb / $kg) * $cb - (2 * (1 - $kr) * $kr / $kg) * $cr
    return @(
        [int][math]::Round((Clamp $r 0 1) * 255),
        [int][math]::Round((Clamp $g 0 1) * 255),
        [int][math]::Round((Clamp $b 0 1) * 255)
    )
}

# 生 YUV420 planar を書く。depth 8 は 1 byte/sample、10 は little-endian 2 byte。
function Write-YuvFrame([string]$path, $patches, [int]$depth) {
    $scale = [math]::Pow(2, $depth - 8)
    $fill = @([int](128 * $scale), [int](128 * $scale), [int](128 * $scale))

    $cw = $Width / 2
    $ch = $Height / 2
    $bytesPerSample = if ($depth -gt 8) { 2 } else { 1 }

    $stream = [System.IO.File]::Create($path)
    try {
        $writer = New-Object System.IO.BinaryWriter($stream)

        # --- Y plane ---
        for ($y = 0; $y -lt $Height; $y++) {
            $row = New-Object byte[] ($Width * $bytesPerSample)
            for ($x = 0; $x -lt $Width; $x++) {
                $v = $fill[0]
                if ($y -lt $PatchSize) {
                    $idx = [math]::Floor($x / $PatchSize)
                    if ($idx -lt $PatchCount) { $v = $patches[$idx].yuv[0] }
                }
                if ($bytesPerSample -eq 1) {
                    $row[$x] = [byte]$v
                } else {
                    $row[$x * 2] = [byte]($v -band 0xFF)
                    $row[$x * 2 + 1] = [byte](($v -shr 8) -band 0xFF)
                }
            }
            $writer.Write($row)
        }

        # --- U / V plane ---
        foreach ($plane in 1, 2) {
            for ($y = 0; $y -lt $ch; $y++) {
                $row = New-Object byte[] ($cw * $bytesPerSample)
                for ($x = 0; $x -lt $cw; $x++) {
                    $v = $fill[$plane]
                    if ($y -lt ($PatchSize / 2)) {
                        $idx = [math]::Floor(($x * 2) / $PatchSize)
                        if ($idx -lt $PatchCount) { $v = $patches[$idx].yuv[$plane] }
                    }
                    if ($bytesPerSample -eq 1) {
                        $row[$x] = [byte]$v
                    } else {
                        $row[$x * 2] = [byte]($v -band 0xFF)
                        $row[$x * 2 + 1] = [byte](($v -shr 8) -band 0xFF)
                    }
                }
                $writer.Write($row)
            }
        }
        $writer.Flush()
    } finally {
        $stream.Dispose()
    }
}

# 生成する fixture。gate は「正式検査の対象か」。
$fixtures = @(
    @{ id = 'bt709_limited';     matrix = 'bt709';     range = 'limited'; depth = 8;  gate = $true  }
    @{ id = 'bt709_full';        matrix = 'bt709';     range = 'full';    depth = 8;  gate = $true  }
    @{ id = 'bt601_limited';     matrix = 'bt601';     range = 'limited'; depth = 8;  gate = $true  }
    @{ id = 'bt2020ncl_limited'; matrix = 'bt2020ncl'; range = 'limited'; depth = 8;  gate = $false }
    @{ id = 'p010_bt709_limited';matrix = 'bt709';     range = 'limited'; depth = 10; gate = $false }
)

# ffmpeg の colorspace 名
$csName = @{ 'bt709' = 'bt709'; 'bt601' = 'smpte170m'; 'bt2020ncl' = 'bt2020nc' }
$prName = @{ 'bt709' = 'bt709'; 'bt601' = 'smpte170m'; 'bt2020ncl' = 'bt2020' }
# transfer characteristics は primaries と名前が違う (bt2020 は bt2020-10)。
$trcName = @{ 'bt709' = 'bt709'; 'bt601' = 'smpte170m'; 'bt2020ncl' = 'bt2020-10' }

$assets = @()
$tmpDir = Join-Path $OutDir '_tmp'
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

foreach ($fx in $fixtures) {
    $outPath = Join-Path $OutDir "$($fx.id).mp4"
    $m = $matrices[$fx.matrix]

    # patch ごとの YUV と期待 RGB を先に決める
    $patches = @()
    for ($i = 0; $i -lt $PatchCount; $i++) {
        $yuv = ConvertTo-Yuv $patchRgb[$i] $m.kr $m.kb $fx.range $fx.depth
        $exp = ConvertTo-ExpectedRgb $yuv $m.kr $m.kb $fx.range $fx.depth
        $patches += [pscustomobject]@{
            index        = $i
            source_rgb   = $patchRgb[$i]
            yuv          = $yuv
            expected_rgb = $exp
        }
    }

    if ((Test-Path $outPath) -and -not $Force) {
        Write-Host "既存のためスキップ: $outPath (再生成は -Force)" -ForegroundColor DarkGray
    } else {
        $rawPath = Join-Path $tmpDir "$($fx.id).yuv"
        Write-Host "生成中: $($fx.id) ($($fx.matrix) / $($fx.range) / $($fx.depth)bit)" -ForegroundColor Yellow
        Write-YuvFrame -path $rawPath -patches $patches -depth $fx.depth

        $pixFmt = if ($fx.depth -gt 8) { 'yuv420p10le' } else { 'yuv420p' }
        # **量子化で patch の値がずれないよう qp を十分低くする。**
        # 平坦な 64x64 ブロックなので、この設定なら DC がそのまま復元される。
        # 完全 lossless (qp=0) は hw decoder が拒否することがあるので使わない。
        $encArgs = if ($fx.depth -gt 8) {
            @('-c:v', 'libx265', '-x265-params', 'qp=4:log-level=error', '-profile:v', 'main10')
        } else {
            @('-c:v', 'libx264', '-qp', '4', '-profile:v', 'high')
        }

        # **入力フレームに色情報を付ける (setparams)。**
        # 付けないと ffmpeg は「入力は BT.601」と仮定し、出力の -colorspace へ
        # 合わせるために colorspace 変換フィルタを自動挿入する。
        # その結果、書き込んだはずの YUV が別の値になって焼かれる。
        # 実測: BT.709 fixture で V=240 と書いたのに 229 で復元された
        # (BT.601 fixture だけ一致していたのは、仮定と一致していたからにすぎない)。
        # setparams は画素を変えずにタグだけ付けるので、変換が挿入されない。
        $rangeTag = $(if ($fx.range -eq 'full') { 'pc' } else { 'tv' })
        $setparams = "setparams=color_primaries=$($prName[$fx.matrix]):" +
                     "color_trc=$($trcName[$fx.matrix]):" +
                     "colorspace=$($csName[$fx.matrix]):range=$rangeTag"

        $args = @(
            '-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'rawvideo', '-pix_fmt', $pixFmt, '-s', "${Width}x${Height}", '-r', '30',
            '-i', $rawPath, '-frames:v', '30', '-vf', $setparams
        ) + $encArgs + @(
            '-pix_fmt', $pixFmt,
            '-colorspace', $csName[$fx.matrix],
            '-color_primaries', $prName[$fx.matrix],
            '-color_trc', $trcName[$fx.matrix],
            '-color_range', $rangeTag,
            $outPath
        )
        & $FFmpeg @args
        if ($LASTEXITCODE -ne 0) { throw "$($fx.id) の生成に失敗しました (exit $LASTEXITCODE)" }
        Remove-Item $rawPath -Force
    }

    $assets += [pscustomobject]@{
        id            = $fx.id
        relative_path = "$($fx.id).mp4"
        gate          = $fx.gate
        matrix        = $fx.matrix
        range         = $fx.range
        depth         = $fx.depth
        width         = $Width
        height        = $Height
        patch_size    = $PatchSize
        patch_count   = $PatchCount
        # 許容誤差。shader は float、RT は 8bit なので厳密一致は要求しない。
        # 3 は「符号ミス・行列取り違え・range 取り違え」を必ず落とせる幅である
        # (それらは 10 以上ずれる)。
        tolerance     = 3
        patches       = $patches
    }
}

Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue

$manifest = [pscustomobject]@{
    schema     = 'mvm-color-fixture-1'
    generated  = (Get-Date).ToString('o')
    note       = '期待 RGB は標準式から独立に計算している。実装の関数は呼んでいない。'
    assets     = $assets
}
$manifestPath = Join-Path $OutDir 'manifest.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $manifestPath

# C++ 側は行指向の manifest を読む。
# JSON parser を検査用 CLI へ足すより、書式を単純にする方が事故が少ない。
# **同じ $assets から両方を出す** ので、値の出所は 1 つである。
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# mvm-color-fixture-1  (scripts/make-color-fixtures.ps1 が生成)')
$lines.Add('# asset <id> <relpath> <gate> <matrix> <range> <depth> <patchSize> <patchCount> <tolerance>')
$lines.Add('# patch <id> <index> <y> <u> <v> <expR> <expG> <expB>')
foreach ($a in $assets) {
    $g = if ($a.gate) { 'gate' } else { 'diag' }
    $lines.Add("asset $($a.id) $($a.relative_path) $g $($a.matrix) $($a.range) $($a.depth) $($a.patch_size) $($a.patch_count) $($a.tolerance)")
    foreach ($p in $a.patches) {
        $lines.Add("patch $($a.id) $($p.index) $($p.yuv[0]) $($p.yuv[1]) $($p.yuv[2]) $($p.expected_rgb[0]) $($p.expected_rgb[1]) $($p.expected_rgb[2])")
    }
}
$txtPath = Join-Path $OutDir 'manifest.txt'
$lines | Set-Content -Encoding UTF8 $txtPath

Write-Host "`n完了: $manifestPath" -ForegroundColor Green
foreach ($a in $assets) {
    $g = if ($a.gate) { '判定対象' } else { '診断のみ' }
    Write-Host ("  {0,-22} {1,-10} {2,-8} {3}bit  {4}" -f $a.id, $a.matrix, $a.range, $a.depth, $g)
}
