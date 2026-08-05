<#
.SYNOPSIS
    all-video proxy が M5 / M8 を満たさなかったときの原因切り分け (S7.1 / I)。

.DESCRIPTION
    「何を外すと速くなるか」を 15 秒 preview で短時間に測る。
    目的は最適化ではなく、**単独の低コスト回避策で 50fps へ届く根拠があるか**を
    判断することだけである。

    ケース:
      A. V1 proxy のみ (V2 は 1080p HEVC original)
      B. V1 + V2 proxy、全 5 トラック  ← 基準
      C. B から qtext を除去
      D. B から音声 2 トラックを除去
      E. B から V2 / affine を除去
      F. B の qtext を **静的 RGBA overlay の代用物**へ置換
         (png_alpha.png。文字画像ではない。視覚的に同等とは主張しない)
      G. V2 proxy を 640x360 にした構成

    **フレームを作らない構成で速く見せることはしない。**
    各ケースは必ず 1920x1080 の RGBA を毎フレーム生成する。
    除去したケースは「その処理が無い場合の上限」であって、
    製品で使える構成とは限らない。それも表に書く。

    private API の cache purge、EmptyWorkingSet、閾値緩和は使わない。

.EXAMPLE
    pwsh scripts/ablation-matrix.ps1
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    # **15 秒では足りない。** 同じ scenario を 15 秒で 3 回測ると
    # 29.5 / 74.6 / 84.5 fps という二峰性が出た (全 run とも
    # rendered=0 なし、position 連番、duplicate なしの正当な計測)。
    # 60 秒にすると preview-matrix と同じ 1〜3% の再現性に収まる。
    # 短い窓は速い側・遅い側のどちらかに丸ごと入ってしまい、
    # 3 回の中央値でも救えない (2 回が速い側に入れば中央値も速い側になる)。
    [int]$MeasureMs = 60000,
    [int]$Runs = 3,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
$ScenDir  = Join-Path $RepoRoot 'bench\scenarios'
$ProxyDir = Join-Path $RepoRoot 'tests\assets\benchmark\_proxy'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\$Preset\ablation" }

# 派生 scenario は **build の下** へ置く。bench/scenarios は入力であり、
# 実行するたびに派生物が湧く場所にしてはいけない。
# 以前はここへ書いていたため abl-*.json が Git に入ってしまった。
#
# 別階層へ置くと media_root ("../../tests/assets/benchmark") がずれる。
# 派生 scenario では media_root を絶対パスへ書き直して回避する。
# 派生物は build の下だけに存在し、コミットされないので絶対パスでよい。
$AblDir = Join-Path $OutDir 'scenarios'
New-Item -ItemType Directory -Force -Path $OutDir, $AblDir | Out-Null
$MediaRootAbs = (Resolve-Path (Join-Path $RepoRoot 'tests\assets\benchmark')).Path -replace '\\', '/'
if (-not (Test-Path $Bench)) { throw "mvm_bench がありません: $Bench" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

$baseAll = Join-Path $ScenDir 's7-4k-proxy-all-gop12.json'
if (-not (Test-Path $baseAll)) { throw "all-video proxy scenario がありません: $baseAll" }

# --- ケース用 scenario を生成する -------------------------------------------
function New-Variant {
    param([string]$Name, [scriptblock]$Mutate)
    $o = Get-Content $baseAll -Raw | ConvertFrom-Json
    & $Mutate $o
    $o.name = $Name
    # 出力先が bench/scenarios ではないので、相対の media_root は使えない。
    $o.media_root = $MediaRootAbs
    $path = Join-Path $AblDir "$Name.json"
    $o | ConvertTo-Json -Depth 20 | Set-Content $path -Encoding UTF8
    return $path
}

# 640x360 の V2 proxy (ケース G 用)。無ければ作る。
$v2small = Join-Path $ProxyDir 'v1080p60_hevc_proxy_360.mp4'
if (-not (Test-Path $v2small)) {
    Write-Host "ケース G 用に 640x360 の V2 proxy を生成します..." -ForegroundColor Cyan
    $src = Join-Path $RepoRoot 'tests\assets\benchmark\v1080p60_hevc.mp4'
    $tmp = "$v2small.mvmtmp"
    & (Join-Path $Ucrt64 'bin\ffmpeg.exe') -hide_banner -v error -y -i $src `
        -vf 'scale=640:360' -c:v h264_nvenc -preset p4 -rc vbr -cq 25 -b:v 0 -g 12 -bf 0 `
        -c:a copy -movflags +faststart -f mp4 $tmp
    if ($LASTEXITCODE -ne 0) { Remove-Item $tmp -ErrorAction SilentlyContinue; throw '640x360 proxy の生成に失敗' }
    Move-Item -Force $tmp $v2small
}

$cases = @(
    @{ Id = 'A'; Desc = 'V1 proxy のみ (V2 は 1080p HEVC original)'
       Scenario = (Join-Path $ScenDir 's7-4k-proxy-gop12.json'); Removed = 'なし'; Usable = 'いいえ (V2 が original)' }
    @{ Id = 'B'; Desc = 'V1 + V2 proxy、全 5 トラック (基準)'
       Scenario = $baseAll; Removed = 'なし'; Usable = 'はい' }
    @{ Id = 'C'; Desc = 'B から qtext を除去'
       Scenario = (New-Variant 'abl-C-no-text' { param($o) $o.tracks = @($o.tracks | Where-Object { $_.kind -ne 'text' }) })
       Removed = '文字レイヤ'; Usable = 'いいえ (文字は必須機能)' }
    @{ Id = 'D'; Desc = 'B から音声 2 トラックを除去'
       Scenario = (New-Variant 'abl-D-no-audio' { param($o) $o.tracks = @($o.tracks | Where-Object { $_.kind -ne 'audio' }) })
       Removed = '音声 A1/A2'; Usable = 'いいえ (音声は必須)' }
    @{ Id = 'E'; Desc = 'B から V2 / affine を除去'
       Scenario = (New-Variant 'abl-E-no-v2' { param($o) $o.tracks = @($o.tracks | Where-Object { $_.name -ne 'V2' }) })
       Removed = 'V2 と affine 合成'; Usable = 'いいえ (PiP は必須)' }
    # **F は「文字を事前描画した画像」ではない。**
    # 使っているのは既存の png_alpha.png であり、文字は入っていない。
    # 測っているのは「qtext の毎フレーム描画を、
    # 同じ位置・同じ大きさの静的 RGBA overlay 1 枚に置き換えた場合の上限」だけである。
    # 実際の文字画像に置き換えたときに同じ fps になる保証は無い
    # (グリフの多さ、alpha の分布、素材の解像度で変わりうる)。
    # 視覚的な同等性はこのケースでは一切確認していない。
    @{ Id = 'F'; Desc = 'B の qtext を静的 RGBA overlay の代用物へ置換 (文字画像ではない)'
       Scenario = (New-Variant 'abl-F-text-png' {
           param($o)
           foreach ($t in $o.tracks) {
               if ($t.kind -eq 'text') {
                   $t.kind = 'video'
                   foreach ($c in $t.clips) {
                       $c | Add-Member -NotePropertyName source -NotePropertyValue 'png_alpha.png' -Force
                       $c | Add-Member -NotePropertyName rect_x -NotePropertyValue 96 -Force
                       $c | Add-Member -NotePropertyName rect_y -NotePropertyValue 240 -Force
                       $c | Add-Member -NotePropertyName rect_w -NotePropertyValue 1500 -Force
                       $c | Add-Member -NotePropertyName rect_h -NotePropertyValue 320 -Force
                       $c | Add-Member -NotePropertyName opacity -NotePropertyValue 1.0 -Force
                       $c | Add-Member -NotePropertyName source_in -NotePropertyValue 0 -Force
                       $c | Add-Member -NotePropertyName source_out -NotePropertyValue 3599 -Force
                   }
               }
           }
       })
       Removed = 'qtext の毎フレーム描画 (静的 RGBA overlay で代用)'
       Usable = '未確認 (代用物での上限。文字画像での等価性は未検証)' }
    @{ Id = 'G'; Desc = 'V2 proxy を 640x360 にした構成'
       Scenario = (New-Variant 'abl-G-v2-360' {
           param($o)
           foreach ($t in $o.tracks) {
               foreach ($c in $t.clips) {
                   if ($c.PSObject.Properties['source'] -and
                       "$($c.source)" -like '*v1080p60_hevc_proxy_gop12*') {
                       $c.source = '_proxy/v1080p60_hevc_proxy_360.mp4'
                   }
               }
           }
       })
       Removed = 'なし (V2 の proxy 解像度のみ変更)'; Usable = 'はい' }
)

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0.0 }
    $s = @($Values | Sort-Object); $n = $s.Count
    if ($n % 2 -eq 1) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

$rows = @()
foreach ($c in $cases) {
    Write-Host "`n=== ケース $($c.Id): $($c.Desc) ===" -ForegroundColor Cyan
    $runObjs = @()
    for ($r = 1; $r -le $Runs; $r++) {
        $json = Join-Path $OutDir "$($c.Id)-run$r.json"
        $log  = Join-Path $OutDir "$($c.Id)-run$r.log"
        $p = Start-Process -FilePath $Bench -NoNewWindow -Wait -PassThru `
            -ArgumentList @('preview-bench', $c.Scenario, '--consumer', 'null',
                            '--real-time', -1, '--measure-ms', $MeasureMs,
                            '--warmup-ms', 3000, '--timeout-ms', 120000, '--json', $json) `
            -RedirectStandardOutput (Join-Path $OutDir "$($c.Id)-run$r.stdout") `
            -RedirectStandardError $log
        if ($p.ExitCode -ne 0) {
            Write-Host "  ケース $($c.Id) run $r が exit $($p.ExitCode) で失敗。$log を参照。" -ForegroundColor Red
            continue
        }
        $o = Get-Content $json -Raw | ConvertFrom-Json
        # 「描画していないのに速い」を弾く。rendered=0 が混じっていたら採用しない。
        if ($o.not_rendered_frames -ne 0) {
            throw "ケース $($c.Id) run ${r}: rendered=0 の frame が $($o.not_rendered_frames) 件"
        }
        if ($o.rendered_frames -le 0) { throw "ケース $($c.Id) run ${r}: 描画 frame が 0" }
        $runObjs += $o
        Write-Host ("  run{0}: fps={1:N2} cpu={2:N2} priv={3:N0}MB" -f `
            $r, $o.effective_fps, $o.cpu_utilization, $o.private_usage_mb.peak)
    }
    # 0 件成功のケースを黙って表から落とさない。
    # 「測れなかった」を「そのケースは無かった」にしてはいけない。
    if ($runObjs.Count -eq 0) {
        throw "ケース $($c.Id) は 1 回も成功しませんでした。$OutDir のログを参照。"
    }
    function M([string]$Path) { Get-Median (@($runObjs | ForEach-Object { [double](Invoke-Expression "`$_.$Path") })) }

    # run 間のばらつきが大きい測定は中央値を取っても意味が無い。
    # 二峰性に当たっていた場合、中央値は「たまたま多かった側」を指すだけである。
    # 中央値を出す前に、run どうしが同じものを測れているかを確認する。
    $caseFps = @($runObjs | ForEach-Object { [double]$_.effective_fps })
    $lo = ($caseFps | Measure-Object -Minimum).Minimum
    $hi = ($caseFps | Measure-Object -Maximum).Maximum
    $spread = if ($lo -gt 0) { [math]::Round($hi / $lo, 3) } else { 0 }
    # 1.25 倍を超えるばらつきは「同じものを測れていない」とみなす。
    # 落とさずに続けるが、**その値は判断に使わない**と表へ書き込む。
    # 中止すると安定して測れているケースまで捨てることになる。
    $stable = ($lo -gt 0 -and $spread -le 1.25)
    if (-not $stable) {
        Write-Host ("  [不安定] run 間が {0:N2}〜{1:N2} fps ({2} 倍)。この値は判断に使わない。" -f `
            $lo, $hi, $spread) -ForegroundColor Yellow
    }

    $rows += [pscustomobject]([ordered]@{
        Case = $c.Id; Desc = $c.Desc
        Fps = [math]::Round((M 'effective_fps'), 2)
        IntervalP50 = [math]::Round((M 'inter_frame_interval_ms.p50'), 2)
        CpuUtil = [math]::Round((M 'cpu_utilization'), 2)
        PrivPeakMb = [math]::Round((M 'private_usage_mb.peak'), 1)
        Rendered = [int](M 'rendered_frames')
        Removed = $c.Removed
        Usable = $c.Usable
        DeltaVsB = 0.0
        Spread = $spread
        Stable = if ($stable) { 'はい' } else { '**いいえ**' }
    })
}

$baseFps = ($rows | Where-Object { $_.Case -eq 'B' } | Select-Object -First 1).Fps
foreach ($r in $rows) { $r.DeltaVsB = [math]::Round($r.Fps - $baseFps, 2) }

# --- 物理的にありえない結果を採用しない ------------------------------------
#
# A は B と同じ合成に対して V2 だけ 1080p HEVC original を使う。
# **A のデコード仕事量は B 以上**であり、A が B より速くなることはない。
# 同様に C / D / E は B から処理を取り除いた構成なので B 以上のはずである。
#
# 実際に、他の作業と並行して流したバックグラウンド実行で
# A=38.17 / B=20.32 という結果が出た。単独で測り直すと A=14.8 / B=19.3 で、
# **A の値だけが再現しなかった。** 計測が汚染されていたということである。
# 数値の大小を見ただけでは気づけないので、機械で弾く。
$fps = @{}
foreach ($r in $rows) { if ($r.Stable -eq 'はい') { $fps[$r.Case] = $r.Fps } }
$violations = New-Object System.Collections.Generic.List[string]
function Assert-NotFaster {
    param([string]$Slower, [string]$Faster, [string]$Why)
    if (-not ($fps.ContainsKey($Slower) -and $fps.ContainsKey($Faster))) { return }
    # 5% の測定ゆらぎは許す。桁で違うものだけを弾く。
    if ($fps[$Slower] -gt $fps[$Faster] * 1.05) {
        $violations.Add(("{0}={1} が {2}={3} より速い。{4}" -f `
            $Slower, $fps[$Slower], $Faster, $fps[$Faster], $Why))
    }
}
Assert-NotFaster 'A' 'B' 'A は V2 に 1080p HEVC original を使う。B 以上に速くなることはない。'
Assert-NotFaster 'B' 'C' 'C は B から qtext を除いた構成である。'
Assert-NotFaster 'B' 'D' 'D は B から音声 2 トラックを除いた構成である。'
Assert-NotFaster 'B' 'E' 'E は B から V2 / affine を除いた構成である。'
if ($violations.Count -ne 0) {
    Write-Host "`n計測結果が物理的に矛盾しています。採用しません:" -ForegroundColor Red
    foreach ($v in $violations) { Write-Host "  - $v" -ForegroundColor Red }
    Write-Host "他の負荷を止めてから測り直してください。" -ForegroundColor Red
    throw "ablation の計測が汚染されています ($($violations.Count) 件)"
}

Write-Host "`n=== ablation (15 秒 preview / real_time=-1 / $Runs runs 中央値) ===" -ForegroundColor Cyan
Write-Host "基準は B (all-video proxy)。DeltaVsB は B との fps 差。"
Write-Host "Stable=いいえ の行は run 間のばらつきが 1.25 倍を超えている。**判断に使わない。**"
$rows | Format-Table Case, Fps, DeltaVsB, Spread, Stable, IntervalP50, CpuUtil, PrivPeakMb, Usable -AutoSize

$rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'ablation.json') -Encoding UTF8

$stableRows = @($rows | Where-Object { $_.Stable -eq 'はい' })
$unstable = @($rows | Where-Object { $_.Stable -ne 'はい' })
if ($unstable.Count -ne 0) {
    Write-Host ("`n測定が不安定で判断に使えないケース: {0}" -f (($unstable | ForEach-Object { $_.Case }) -join ', ')) `
        -ForegroundColor Yellow
}
if ($stableRows.Count -eq 0) { throw '安定して測れたケースが 1 つもありません。' }
$best = ($stableRows | Sort-Object -Property Fps -Descending | Select-Object -First 1)
Write-Host "`n最速ケース (安定して測れたものだけ): $($best.Case) = $($best.Fps) fps (製品で使えるか: $($best.Usable))"
if ($best.Fps -lt 50) {
    Write-Host "**すべて除去しても 50fps に届かない。単独の低コスト回避策では埋まらない。**" -ForegroundColor Yellow
}
Write-Host "生 JSON: $OutDir"
