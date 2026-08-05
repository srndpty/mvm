<#
.SYNOPSIS
    4K の logical timeline から original / proxy の scenario を生成する (S7 / S7.1)。

.DESCRIPTION
    同じ logical timeline から次の 3 種を生成する。

      s7-4k-original.json        4K をそのまま。native 基準
      s7-4k-proxy-gop12.json     V1 だけ proxy。**partial proxy diagnostic**
      s7-4k-proxy-all-gop12.json V1 と V2 の両方を proxy。**正式 M8 評価**

    s7-4k-proxy-gop1.json は encoder 違い (libx264) の比較用 diagnostic として
    proxy が存在する場合にのみ生成する。判定には使わない。

    差し替えは **MLT graph を構築する前** に scenario 上で行う。
    proxy 情報を MLT XML にも MLT property にも持たせない。
    解決そのものは mvm_bench resolve-proxy (tests/harness/proxy_resolver.h) が行う。

    final 用は必ず original を返すので、ここでは preview 用だけを生成する。
    final の scenario は original scenario と同一である。

    **再現性**: clean state (proxy scenario が 1 つも無い状態) から実行しても
    同じ 3 ファイルが出る。既存ファイルの内容には依存しない。

.EXAMPLE
    pwsh scripts/make-proxy-scenarios.ps1
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
$ScenDir  = Join-Path $RepoRoot 'bench\scenarios'
$ProxyDir = Join-Path $RepoRoot 'tests\assets\benchmark\_proxy'
if (-not (Test-Path $Bench)) { throw "mvm_bench がありません: $Bench" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

# preview で使う video source。両方 proxy 化して初めて「proxy の上限」になる。
$V1 = 'v4k60_h264.mp4'
$V2 = 'v1080p60_hevc.mp4'
# A1 / A2 は音声専用の optional source。proxy 化しない。
$A1 = 'v1080p60_h264.mp4'
$A2 = 'wav_48k.wav'

$V2Proxy    = '_proxy/v1080p60_hevc_proxy_gop12.mp4'
$V2ProxyAbs = Join-Path $ProxyDir 'v1080p60_hevc_proxy_gop12.mp4'

# --- logical timeline (4K 版) -----------------------------------------------
# 構造は S5 / S7 の 5 トラックと同じ。V1 を 4K にしたもの。
$src = Get-Content (Join-Path $ScenDir 's7-five-track-60s.json') -Raw | ConvertFrom-Json

$orig = $src | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$orig.name = 's7-4k-original'
$orig.'//' = 'S7 の 4K logical timeline (original)。preview 用の proxy 版は resolve-proxy が生成する。'
$orig.tracks[0].clips[0].source = $V1

$origPath = Join-Path $ScenDir 's7-4k-original.json'
$orig | ConvertTo-Json -Depth 20 | Set-Content $origPath -Encoding UTF8
Write-Host "書き出し: $origPath"

# resolve-proxy の JSON 報告を読む。
function Invoke-Resolve {
    param([string[]]$ChildArgs, [int]$ExpectExit = 0)
    $out = & $Bench @ChildArgs 2>&1
    $code = $LASTEXITCODE
    if ($code -ne $ExpectExit) {
        Write-Host ($out | Out-String)
        throw "resolve-proxy の exit が $code (期待 $ExpectExit): $($ChildArgs -join ' ')"
    }
    # JSON 部分だけを取り出す ('{' 行から最後の '}' 行まで)。
    $text = ($out | Out-String)
    $s = $text.IndexOf('{')
    $e = $text.LastIndexOf('}')
    if ($s -lt 0 -or $e -le $s) { throw "resolve-proxy の JSON 報告を読めません" }
    return $text.Substring($s, $e - $s + 1) | ConvertFrom-Json
}

function Set-ScenarioName {
    param([string]$Path, [string]$Name, [string]$Note)
    $o = Get-Content $Path -Raw | ConvertFrom-Json
    $o.name = $Name
    $o.'//' = $Note
    $o | ConvertTo-Json -Depth 20 | Set-Content $Path -Encoding UTF8
}

function Assert-Equal {
    param($Actual, $Expected, [string]$What)
    if ("$Actual" -ne "$Expected") { throw "検査失敗 [$What]: 実測 '$Actual' / 期待 '$Expected'" }
    Write-Host ("  OK  {0} = {1}" -f $What, $Actual)
}

# --- 1) partial proxy diagnostic (V1 のみ) ----------------------------------
# **これは正式な M8 評価ではない。** V2 は original のまま残る。
foreach ($cand in @('gop12', 'gop1')) {
    $proxyAbs = Join-Path $ProxyDir "v4k60_h264_proxy_$cand.mp4"
    if (-not (Test-Path $proxyAbs)) {
        Write-Host "proxy がまだありません ($cand)。先に pwsh scripts/proxy-matrix.ps1 を実行してください。" -ForegroundColor Yellow
        continue
    }
    $out = Join-Path $ScenDir "s7-4k-proxy-$cand.json"
    $rel = "_proxy/v4k60_h264_proxy_$cand.mp4"

    Write-Host "`n--- partial proxy diagnostic ($cand): V1 のみ ---"
    $r = Invoke-Resolve @('resolve-proxy', $origPath, '--target', 'preview',
                          '--map', "$V1=$rel", '--require-proxy-ids', $V1, '--out', $out)
    Assert-Equal $r.required_proxy_ids.Count 1 "required_proxy_ids 件数 ($cand)"
    Assert-Equal $r.resolved_required_ids.Count 1 "resolved_required_ids 件数 ($cand)"
    Assert-Equal $r.missing_required_ids.Count 0 "missing_required_ids 件数 ($cand)"
    Assert-Equal $r.resolved_occurrences 1 "resolved_occurrences ($cand)"

    Set-ScenarioName $out "s7-4k-proxy-$cand" `
        "**partial proxy diagnostic**: V1 のみ proxy。V2 は original。正式な M8 評価に使わない。"
    Write-Host "書き出し: $out"
}

# --- 2) 正式 M8: all-video proxy (V1 + V2) ----------------------------------
$V1Proxy = '_proxy/v4k60_h264_proxy_gop12.mp4'
$V1ProxyAbs = Join-Path $ProxyDir 'v4k60_h264_proxy_gop12.mp4'
if (-not ((Test-Path $V1ProxyAbs) -and (Test-Path $V2ProxyAbs))) {
    throw "all-video proxy に必要な proxy がありません。先に pwsh scripts/proxy-matrix.ps1 を実行してください。`n  $V1ProxyAbs`n  $V2ProxyAbs"
}

$allOut = Join-Path $ScenDir 's7-4k-proxy-all-gop12.json'
$map2 = "$V1=$V1Proxy;$V2=$V2Proxy"
$req2 = "$V1;$V2"

Write-Host "`n--- 正式 M8: all-video proxy (V1 + V2) ---"
$r = Invoke-Resolve @('resolve-proxy', $origPath, '--target', 'preview',
                      '--map', $map2, '--require-proxy-ids', $req2, '--out', $allOut)

# (1) required は 2 件
Assert-Equal $r.required_proxy_ids.Count 2 'required_proxy_ids 件数'
# (2) resolved も 2 件
Assert-Equal $r.resolved_required_ids.Count 2 'resolved_required_ids 件数'
# (3) missing は 0 件
Assert-Equal $r.missing_required_ids.Count 0 'missing_required_ids 件数'
# (4) 置換した出現数。V1 / V2 が 1 clip ずつなので 2
Assert-Equal $r.resolved_occurrences 2 'resolved_occurrences'

Set-ScenarioName $allOut 's7-4k-proxy-all-gop12' `
    '**正式 M8 評価**: preview で使う video source (V1 / V2) を全て proxy 化した scenario。'

$allJson = Get-Content $allOut -Raw | ConvertFrom-Json

# (5) V1 / V2 の source が proxy パスになっている
Assert-Equal $allJson.tracks[0].clips[0].source $V1Proxy 'V1 の source'
Assert-Equal $allJson.tracks[1].clips[0].source $V2Proxy 'V2 の source'

# (6) A1 / A2 は original のまま
Assert-Equal $allJson.tracks[2].clips[0].source $A1 'A1 の source (original のまま)'
Assert-Equal $allJson.tracks[3].clips[0].source $A2 'A2 の source (original のまま)'

Write-Host "書き出し: $allOut"

# (7) final は proxy パスを 1 件も含まない。生成のたびに機械で確認する。
Write-Host "`n--- final の確認 ---"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) 'mvm-final-check-all.json'
Remove-Item $tmp -ErrorAction SilentlyContinue
$rf = Invoke-Resolve @('resolve-proxy', $origPath, '--target', 'final',
                       '--map', $map2, '--require-proxy-ids', $req2, '--out', $tmp)
Assert-Equal $rf.resolved_to_proxy 0 'final の resolved_to_proxy'
$finalText = Get-Content $tmp -Raw
$hits = @(@($V1Proxy, $V2Proxy) | Where-Object { $finalText -match [regex]::Escape($_) })
if ($hits.Count -ne 0) { throw "final の scenario に proxy パスが混入しています: $($hits -join ', ')" }
Write-Host "  OK  final に proxy パスが 0 件"
Remove-Item $tmp -ErrorAction SilentlyContinue

Write-Host "`nfinal 用の scenario は original scenario と同一です ($origPath)。"
Write-Host "正式な M8 評価には s7-4k-proxy-all-gop12.json だけを使うこと。" -ForegroundColor Cyan
